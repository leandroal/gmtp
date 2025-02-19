/* GStreamer
 * Copyright (C) <2007> Leandro Melo de Sales <leandroal@gmail.com>
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Library General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Library General Public License for more details.
 *
 * You should have received a copy of the GNU Library General Public
 * License along with this library; if not, write to the
 * Free Software Foundation, Inc., 51 Franklin St, Fifth Floor,
 * Boston, MA 02110-1301, USA.
 */

/**
 * SECTION:element-gmtpserversink
 * @see_also: gmtpclientsink, gmtpclientsrc, gmtpserversrc
 *
 * This element wait for connections from clients and send data to them.
 * <ulink url="http://www.linuxfoundation.org/en/Net:GMTP">GMTP</ulink> (Datagram
 * Congestion Control Protocol) is a Transport Layer protocol like
 * TCP and UDP.
 *
 * <refsect2>
 * <title>Example pipeline</title>
 * <para>
 * |[
 * gst-launch -v gmtpclientsrc host=localhost port=9011 ccid=2 ! decodebin ! alsasink
 * ]| Client
 * |[
 * gst-launch -v filesrc location=music.mp3 ! mp3parse ! gmtpserversink port=9011 ccid=2
 * ]| Server
 *
 * This example pipeline will send a MP3 stream to the client using GMTP.
 * The client will decode the MP3 and play it. Run the server pipeline
 * first than the client pipeline. If you want, you can run more than one gmtpclientsrc
 * to connect to the same server (see wait-connections property at gmtpserversink).
 * </para>
 * </refsect2>
 */


#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "gstgmtpserversink.h"
#include "gstgmtp.h"
#include <fcntl.h>

/* signals */
enum
{
  SIGNAL_CONNECTED,
  LAST_SIGNAL
};

/* properties */
enum
{
  PROP_0,
  PROP_PORT,
  PROP_CLIENT_SOCK_FD,
 /* PROP_CCID,*/
  PROP_CLOSED,
  PROP_WAIT_CONNECTIONS
};

static pthread_t accept_thread_id;

static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static gboolean gst_gmtp_server_sink_stop (GstBaseSink * bsink);

GST_DEBUG_CATEGORY_STATIC (gmtpserversink_debug);

static GstStaticPadTemplate sinktemplate = GST_STATIC_PAD_TEMPLATE ("sink",
    GST_PAD_SINK,
    GST_PAD_ALWAYS,
    GST_STATIC_CAPS_ANY);

#define gst_gmtp_server_sink_parent_class parent_class
G_DEFINE_TYPE (GstGMTPServerSink, gst_gmtp_server_sink, GST_TYPE_BASE_SINK);

static guint gst_gmtp_server_sink_signals[LAST_SIGNAL] = { 0 };

/*
 * Create a new client with the socket and the MTU
 *
 * @param element - the gstgmtpserversink instance
 * @param socket - the client socket
 * @return the client
 */
static Client *
gst_gmtp_server_create_client (GstElement * element, int socket)
{
  Client *client = (Client *) g_malloc (sizeof (Client));
  client->socket = socket;
  client->pksize = gst_gmtp_get_max_packet_size (element, client->socket);
  client->flow_status = GST_FLOW_OK;

  GST_INFO ("Creating a new client with fd %d and MTU %d.",
      client->socket, client->pksize);

  /* the socket is connected */
  g_signal_emit (element, gst_gmtp_server_sink_signals[SIGNAL_CONNECTED], 0,
      socket);

  return client;
}

/*
 * Wait connections of new clients
 *
 * @param arg - the gstgmtpserversink instance
 */
static void *
gst_gmtp_server_accept_new_clients (void *arg)
{
  GstGMTPServerSink *sink = (GstGMTPServerSink *) arg;
  int newsockfd;
  Client *client;

  while (1) {
    newsockfd =
        gst_gmtp_server_wait_connections (GST_ELEMENT (sink), sink->sock_fd);

    client = gst_gmtp_server_create_client (GST_ELEMENT (sink), newsockfd);

    pthread_mutex_lock (&lock);
    sink->clients = g_list_append (sink->clients, client);
    pthread_mutex_unlock (&lock);
  }

  return NULL;
}

/*
 * Send the buffer to a client
 *
 * @param arg - the client
 */
static void *
gst_gmtp_server_send_buffer (void *arg)
{
  Client *client = (Client *) arg;
  GstGMTPServerSink *sink = client->server;
  GstBuffer *buf = client->buf;
  int client_sock_fd = client->socket;
  int pksize = client->pksize;

  if (gst_gmtp_send_buffer (GST_ELEMENT (sink), buf, client_sock_fd,
          pksize) == GST_FLOW_ERROR) {
    client->flow_status = GST_FLOW_ERROR;
  }
  return NULL;
}

/* Remove clients with problems to send.
 *
 * @param arg - the gstgmtpserversink instance
 */
static void *
gst_gmtp_server_delete_dead_clients (void *arg)
{
  GstGMTPServerSink *sink = (GstGMTPServerSink *) arg;
  GList *tmp = NULL;
  GList *l;

  pthread_mutex_lock (&lock);
  for (l = sink->clients; l != NULL; l = l->next) {
    Client *client = (Client *) l->data;

    if (client->flow_status == GST_FLOW_OK) {
      tmp = g_list_append (tmp, client);
    } else {
      close (client->socket);
      g_free (client);
    }
  }
  g_list_free (sink->clients);
  sink->clients = tmp;
  pthread_mutex_unlock (&lock);
  return 0;
}

static void
gst_gmtp_server_sink_init (GstGMTPServerSink * this)
{
  this->port = GMTP_DEFAULT_PORT;
  this->sock_fd = GMTP_DEFAULT_SOCK_FD;
  this->client_sock_fd = GMTP_DEFAULT_CLIENT_SOCK_FD;
  this->closed = GMTP_DEFAULT_CLOSED;
  /*this->ccid = GMTP_DEFAULT_CCID;*/
  this->wait_connections = GMTP_DEFAULT_WAIT_CONNECTIONS;
  this->clients = NULL;
}

/*
 * Starts the element. If the sockfd property was not the default, this method
 * will wait for a client connection.  If wait-connections property is true, it
 * creates a thread to wait for new client connections.
 *
 * @param bsink - the element
 * @return TRUE if the send operation was successful, FALSE otherwise.
 */
static gboolean
gst_gmtp_server_sink_start (GstBaseSink * bsink)
{
  GstGMTPServerSink *sink = GST_GMTP_SERVER_SINK (bsink);
  Client *client;

  if ((sink->sock_fd = gst_gmtp_create_new_socket (GST_ELEMENT (sink))) < 0) {
    return FALSE;
  }
  GST_LOG_OBJECT (GST_ELEMENT(sink), "SOCKET CRIADO");

  if (!gst_gmtp_make_address_reusable (GST_ELEMENT (sink), sink->sock_fd)) {
    return FALSE;
  }

  /* name the server socket */
  memset (&sink->server_sin, 0, sizeof (sink->server_sin));
  sink->server_sin.sin_family = AF_INET;        /* network socket */
  sink->server_sin.sin_port = htons (sink->port);       /* on port */
  sink->server_sin.sin_addr.s_addr = htonl (INADDR_ANY);        /* for hosts */

  if (!gst_gmtp_bind_server_socket (GST_ELEMENT (sink), sink->sock_fd,
          sink->server_sin)) {
    return FALSE;
  }

 /* if (!gst_gmtp_set_ccid (GST_ELEMENT (sink), sink->sock_fd, sink->ccid)) {
    return FALSE;
  }*/

  if (!gst_gmtp_listen_server_socket (GST_ELEMENT (sink), sink->sock_fd)) {
    return FALSE;
  }


  if (sink->client_sock_fd == GMTP_DEFAULT_CLIENT_SOCK_FD) {
    sink->client_sock_fd =
        gst_gmtp_server_wait_connections (GST_ELEMENT (sink), sink->sock_fd);
  }

  if (sink->client_sock_fd == -1) {
    return FALSE;
  }

  client =
      gst_gmtp_server_create_client (GST_ELEMENT (sink), sink->client_sock_fd);
  sink->clients = g_list_append (sink->clients, client);

  pthread_mutex_init (&lock, NULL);

  if (sink->wait_connections == TRUE) {
    pthread_create (&accept_thread_id, NULL, gst_gmtp_server_accept_new_clients,
        sink);
    pthread_detach (accept_thread_id);
  }

  return TRUE;
}

static GstFlowReturn
gst_gmtp_server_sink_render (GstBaseSink * bsink, GstBuffer * buf)
{
  GstGMTPServerSink *sink = GST_GMTP_SERVER_SINK (bsink);

  pthread_t thread_id;
  GList *l;

  pthread_mutex_lock (&lock);

  for (l = sink->clients; l != NULL; l = l->next) {
    Client *client = (Client *) l->data;

    client->buf = buf;
    client->server = sink;

    /* FIXME: are we really creating a new thread here for every single buffer
     * and every single client? */
    if (client->flow_status == GST_FLOW_OK) {
      pthread_create (&thread_id, NULL, gst_gmtp_server_send_buffer,
          (void *) client);
      pthread_detach (thread_id);
    } else {
      /* FIXME: what's the point of doing this in a separate thread if it
       * keeps he global lock anyway while going through all the clients and
       * waiting for close() to finish? */
      pthread_create (&thread_id, NULL, gst_gmtp_server_delete_dead_clients,
          (void *) sink);
      pthread_detach (thread_id);
    }
  }

  pthread_mutex_unlock (&lock);
  return GST_FLOW_OK;
}

static gboolean
gst_gmtp_server_sink_stop (GstBaseSink * bsink)
{
  GstGMTPServerSink *sink;
  GList *l;

  sink = GST_GMTP_SERVER_SINK (bsink);

  if (sink->wait_connections == TRUE) {
    pthread_cancel (accept_thread_id);
  }

  gst_gmtp_socket_close (GST_ELEMENT (sink), &(sink->sock_fd));

  pthread_mutex_lock (&lock);
  for (l = sink->clients; l != NULL; l = l->next) {
    Client *client = (Client *) l->data;

    if (client->socket != GMTP_DEFAULT_CLIENT_SOCK_FD && sink->closed == TRUE) {
      gst_gmtp_socket_close (GST_ELEMENT (sink), &(client->socket));
    }
    g_free (client);
  }
  pthread_mutex_unlock (&lock);

  return TRUE;
}

/*
 * Set the value of a property for the server sink.
 */
static void
gst_gmtp_server_sink_set_property (GObject * object, guint prop_id,
    const GValue * value, GParamSpec * pspec)
{
  GstGMTPServerSink *sink = GST_GMTP_SERVER_SINK (object);

  switch (prop_id) {
    case PROP_PORT:
      sink->port = g_value_get_int (value);
      break;
    case PROP_CLIENT_SOCK_FD:
      sink->client_sock_fd = g_value_get_int (value);
      break;
    case PROP_CLOSED:
      sink->closed = g_value_get_boolean (value);
      break;
    case PROP_WAIT_CONNECTIONS:
      sink->wait_connections = g_value_get_boolean (value);
      break;
    /*case PROP_CCID:
      sink->ccid = g_value_get_int (value);*/
      break;
    default:
      break;
  }
}

static void
gst_gmtp_server_sink_get_property (GObject * object, guint prop_id,
    GValue * value, GParamSpec * pspec)
{
  GstGMTPServerSink *sink = GST_GMTP_SERVER_SINK (object);

  switch (prop_id) {
    case PROP_PORT:
      g_value_set_int (value, sink->port);
      break;
    case PROP_CLIENT_SOCK_FD:
      g_value_set_int (value, sink->client_sock_fd);
      break;
    case PROP_CLOSED:
      g_value_set_boolean (value, sink->closed);
      break;
    case PROP_WAIT_CONNECTIONS:
      g_value_set_boolean (value, sink->wait_connections);
      break;
    /*case PROP_CCID:
      g_value_set_int (value, sink->ccid);
      break;*/
    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
      break;
  }
}

static GstCaps *
gst_gmtp_server_sink_getcaps (GstBaseSink *bsink, GstCaps * filter)
{
  GstGMTPServerSink *sink;
  GstCaps *caps = NULL;

  sink = GST_GMTP_SERVER_SINK (bsink);

  caps = (filter ? gst_caps_ref (filter) : gst_caps_new_any ());

  GST_DEBUG_OBJECT (sink, "returning caps %" GST_PTR_FORMAT, caps);
  g_assert (GST_IS_CAPS (caps));
  return caps;
}

static gboolean
gst_gmtp_server_sink_setcaps (GstBaseSink * bsink, GstCaps * caps)
{
  GstGMTPServerSink *sink;
  sink = GST_GMTP_SERVER_SINK (bsink);
  GST_INFO("CHAMOUT SET CAPS DE SERVER SINK");
  if ((caps != NULL) && (!gst_caps_is_equal (caps, GST_CAPS_ANY))) {
    if (!gst_pad_set_caps(bsink->sinkpad, caps)) {
       GST_ELEMENT_ERROR (sink, CORE, NEGOTIATION, (NULL),
          ("Error setting caps."));
      return FALSE;
    }
  }
  return TRUE;
}

static void
gst_gmtp_server_sink_class_init (GstGMTPServerSinkClass * klass)
{
  GObjectClass *gobject_class;
  GstBaseSinkClass *gstbasesink_class;
  GstElementClass *gstelement_class;

  gobject_class = (GObjectClass *) klass;
  gstbasesink_class = (GstBaseSinkClass *) klass;
  gstelement_class = (GstElementClass *) klass;

  gobject_class->set_property = gst_gmtp_server_sink_set_property;
  gobject_class->get_property = gst_gmtp_server_sink_get_property;

  gst_element_class_add_pad_template (gstelement_class,
      gst_static_pad_template_get (&sinktemplate));

  gst_element_class_set_static_metadata (gstelement_class, "GMTP server sink",
      "Sink/Network",
      "Send data as a server over the network via GMTP",
      "E-Phone Team at Federal University of Campina Grande <leandroal@gmail.com>");

  g_object_class_install_property (G_OBJECT_CLASS (klass), PROP_PORT,
      g_param_spec_int ("port", "Port",
          "The port to listen to", 0, G_MAXUINT16,
          GMTP_DEFAULT_PORT, G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));

  g_object_class_install_property (gobject_class, PROP_CLIENT_SOCK_FD,
      g_param_spec_int ("sockfd", "Socket fd",
          "The client socket file descriptor", -1, G_MAXINT,
          GMTP_DEFAULT_CLIENT_SOCK_FD,
          G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));

  g_object_class_install_property (gobject_class, PROP_CLOSED,
      g_param_spec_boolean ("close-socket", "Close",
          "Close the client sockets at end of stream",
          GMTP_DEFAULT_CLOSED, G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));

  /*g_object_class_install_property (gobject_class, PROP_CCID,
      g_param_spec_int ("ccid", "CCID",
          "The Congestion Control IDentified to be used", 2, G_MAXINT,
          GMTP_DEFAULT_CCID, G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));*/

  g_object_class_install_property (gobject_class, PROP_WAIT_CONNECTIONS,
      g_param_spec_boolean ("wait-connections", "Wait connections",
          "Wait for many client connections",
          GMTP_DEFAULT_WAIT_CONNECTIONS,
          G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));


  /* signals */
  /**
   * GstDccpServerSink::connected:
   * @sink: the gstgmtpserversink element that emitted this signal
   * @fd: the connected socket file descriptor
   *
   * Reports that the element has connected, giving the fd of the socket
   */
  gst_gmtp_server_sink_signals[SIGNAL_CONNECTED] =
      g_signal_new ("connected", G_TYPE_FROM_CLASS (klass),
      G_SIGNAL_RUN_FIRST,
      G_STRUCT_OFFSET (GstGMTPServerSinkClass, connected), NULL, NULL,
      g_cclosure_marshal_generic, G_TYPE_NONE, 1, G_TYPE_INT);

  gstbasesink_class->start = gst_gmtp_server_sink_start;
  gstbasesink_class->stop = gst_gmtp_server_sink_stop;
  gstbasesink_class->render = gst_gmtp_server_sink_render;
//  gstbasesink_class->set_caps = gst_gmtp_server_sink_setcaps;
  gstbasesink_class->get_caps = gst_gmtp_server_sink_getcaps;

  GST_DEBUG_CATEGORY_INIT (gmtpserversink_debug, "gmtpserversink", 0,
      "GMTP Server Sink");
}

