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
 * SECTION:element-gmtpclientsrc
 * @see_also: gmtpclientsink, gmtpserversink, gmtpserversrc
 *
 * This element connect to a GMTP server and send data to it.
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

#include "gstgmtpclientsrc.h"
#include "gstgmtp.h"
#include <fcntl.h>
#ifdef HAVE_FIONREAD_IN_SYS_FILIO
#include <sys/filio.h>
#endif

#define GMTP_DEFAULT_CAPS       NULL

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
  PROP_HOST,
  PROP_SOCK_FD,
  PROP_CLOSED,
 /* PROP_CCID,*/
  PROP_CAPS
};

static gboolean gst_gmtp_client_src_stop (GstBaseSrc * bsrc);

GST_DEBUG_CATEGORY_STATIC (gmtpclientsrc_debug);

static GstStaticPadTemplate srctemplate = GST_STATIC_PAD_TEMPLATE ("src",
    GST_PAD_SRC,
    GST_PAD_ALWAYS,
    GST_STATIC_CAPS_ANY);

#define gst_gmtp_client_src_parent_class parent_class
G_DEFINE_TYPE (GstGMTPClientSrc, gst_gmtp_client_src, GST_TYPE_PUSH_SRC);

static guint gst_gmtp_client_src_signals[LAST_SIGNAL] = { 0 };

/*
 * Read a buffer from the client socket
 *
 * @return GST_FLOW_OK if the send operation was successful, GST_FLOW_ERROR otherwise.
 */
static GstFlowReturn
gst_gmtp_client_src_create (GstPushSrc * psrc, GstBuffer ** outbuf)
{
  GstGMTPClientSrc *src;
  GstFlowReturn ret = GST_FLOW_OK;

  src = GST_GMTP_CLIENT_SRC (psrc);

  GST_LOG_OBJECT (src, "reading a buffer");
  ret = gst_gmtp_read_buffer (GST_ELEMENT (src), src->sock_fd, outbuf);

  if (ret == GST_FLOW_OK) {
    GST_INFO (
        "Returning buffer from _get of size %" G_GSIZE_FORMAT ", ts %"
        GST_TIME_FORMAT ", dur %" GST_TIME_FORMAT
        ", offset %" G_GINT64_FORMAT ", offset_end %" G_GINT64_FORMAT,
        gst_buffer_get_size (*outbuf),
        GST_TIME_ARGS (GST_BUFFER_TIMESTAMP (*outbuf)),
        GST_TIME_ARGS (GST_BUFFER_DURATION (*outbuf)),
        GST_BUFFER_OFFSET (*outbuf), GST_BUFFER_OFFSET_END (*outbuf));

/*
    if ((src->caps != NULL) && !gst_caps_is_equal (src->caps, GST_CAPS_ANY)) {
http://gstreamer-devel.966125.n4.nabble.com/Problem-with-several-gst-caps-functions-td4663327.html

There is no GstController anymore, the functionality was merged into the 
GstObject base class. 

There are also no caps on the buffers anymore, you need to set them on 
the pad, or more precisely send a caps event downstream.

      //gst_buffer_set_caps (*outbuf, src->caps);
      gst_pad_set_caps(GST_BASE_SRC (src)->srcpad, src->caps);
    }
*/
  }

  return ret;
}

/*
 * Set the value of a property for the client src.
 */
static void
gst_gmtp_client_src_set_property (GObject * object, guint prop_id,
    const GValue * value, GParamSpec * pspec)
{
  GstGMTPClientSrc *src = GST_GMTP_CLIENT_SRC (object);

  switch (prop_id) {
    case PROP_PORT:
      src->port = g_value_get_int (value);
      break;
    case PROP_HOST:
      if (!g_value_get_string (value)) {
        g_warning ("host property cannot be NULL");
        break;
      }
      g_free (src->host);
      src->host = g_strdup (g_value_get_string (value));
      break;
    case PROP_SOCK_FD:
      src->sock_fd = g_value_get_int (value);
      break;
    case PROP_CLOSED:
      src->closed = g_value_get_boolean (value);
      break;
  /*  case PROP_CCID:
      src->ccid = g_value_get_int (value);
      break;*/
    case PROP_CAPS:
    {
      const GstCaps *new_caps_val = gst_value_get_caps (value);
      GstCaps *new_caps;
      GstCaps *old_caps;

      if (new_caps_val == NULL) {
        new_caps = gst_caps_new_any ();
      } else {
        new_caps = gst_caps_copy (new_caps_val);
      }

      old_caps = src->caps;
      src->caps = new_caps;
      if (old_caps)
        gst_caps_unref (old_caps);
      if (new_caps != NULL)
        gst_pad_set_caps (GST_BASE_SRC (src)->srcpad, new_caps);
      break;
    }
    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
      break;
  }
}

/*
 * Get a given property value for the client src.
 */
static void
gst_gmtp_client_src_get_property (GObject * object, guint prop_id,
    GValue * value, GParamSpec * pspec)
{
  GstGMTPClientSrc *src = GST_GMTP_CLIENT_SRC (object);

  switch (prop_id) {
    case PROP_PORT:
      g_value_set_int (value, src->port);
      break;
    case PROP_HOST:
      g_value_set_string (value, src->host);
      break;
    case PROP_SOCK_FD:
      g_value_set_int (value, src->sock_fd);
      break;
    case PROP_CLOSED:
      g_value_set_boolean (value, src->closed);
      break;
   /* case PROP_CCID:
      g_value_set_int (value, src->ccid);
      break;*/
    case PROP_CAPS:
      gst_value_set_caps (value, src->caps);
      break;
    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
      break;
  }
}

/*
 * Starts the element. If the sockfd property was not the default, this method
 * will create a new socket and connect to the server.
 *
 * @param bsrc - the element
 * @return TRUE if the send operation was successful, FALSE otherwise.
 */
static gboolean
gst_gmtp_client_src_start (GstBaseSrc * bsrc)
{
  GstGMTPClientSrc *src = GST_GMTP_CLIENT_SRC (bsrc);

  if (src->sock_fd == GMTP_DEFAULT_SOCK_FD) {
    gchar *ip = NULL;

    /* look up name if we need to */
    if (!(ip = gst_gmtp_host_to_ip (GST_ELEMENT (src), src->host))) {
      GST_ERROR_OBJECT (src, "cannot resolve hostname");
      gst_gmtp_client_src_stop (GST_BASE_SRC (src));
      return FALSE;
    }

    /* name the server socket */
    memset (&src->server_sin, 0, sizeof (src->server_sin));
    src->server_sin.sin_family = AF_INET;       /* network socket */
    src->server_sin.sin_addr.s_addr = inet_addr (ip);   /* on host ip */
    src->server_sin.sin_port = htons (src->port);       /* on port */
    g_free (ip);

    /* create socket */
    if ((src->sock_fd = gst_gmtp_create_new_socket (GST_ELEMENT (src))) < 0) {
      return FALSE;
    }

   /* if (!gst_gmtp_set_ccid (GST_ELEMENT (src), src->sock_fd, src->ccid)) {
      gst_gmtp_client_src_stop (GST_BASE_SRC (src));
      return FALSE;
    }*/

    if (!gst_gmtp_connect_to_server (GST_ELEMENT (src), src->server_sin,
            src->sock_fd)) {
      gst_gmtp_client_src_stop (GST_BASE_SRC (src));
      return FALSE;
    }

    /* the socket is connected */
    g_signal_emit (src, gst_gmtp_client_src_signals[SIGNAL_CONNECTED], 0,
        src->sock_fd);
  }

  return TRUE;
}

static void
gst_gmtp_client_src_init (GstGMTPClientSrc * this)
{
  this->port = GMTP_DEFAULT_PORT;
  this->host = g_strdup (GMTP_DEFAULT_HOST);
  this->sock_fd = GMTP_DEFAULT_SOCK_FD;
  this->closed = GMTP_DEFAULT_CLOSED;
  /*this->ccid = GMTP_DEFAULT_CCID;*/
  this->caps = NULL;

  gst_base_src_set_format (GST_BASE_SRC (this), GST_FORMAT_TIME);

  /* Checking if the version of the gstreamer is bigger that 0.10.15 */
#if ((GST_VERSION_MAJOR > 0) || \
  (GST_VERSION_MAJOR == 0 && GST_VERSION_MINOR > 10) || \
  (GST_VERSION_MAJOR == 0 && GST_VERSION_MINOR == 10 && GST_VERSION_MICRO >= 15))
  gst_base_src_set_do_timestamp (GST_BASE_SRC (this), TRUE);
#endif

  /* FIXME is this correct? */
  gst_base_src_set_live (GST_BASE_SRC (this), TRUE);
}

static void
gst_gmtp_client_src_finalize (GObject * gobject)
{
  GstGMTPClientSrc *this = GST_GMTP_CLIENT_SRC (gobject);

  if (this->caps) {
    gst_caps_unref (this->caps);
    this->caps = NULL;
  }

  g_free (this->host);

  G_OBJECT_CLASS (parent_class)->finalize (gobject);
}

static gboolean
gst_gmtp_client_src_stop (GstBaseSrc * bsrc)
{
  GstGMTPClientSrc *src;

  src = GST_GMTP_CLIENT_SRC (bsrc);

  if (src->sock_fd != GMTP_DEFAULT_SOCK_FD && src->closed) {
    gst_gmtp_socket_close (GST_ELEMENT (src), &(src->sock_fd));
  }

  return TRUE;
}

static GstCaps *
gst_gmtp_client_src_getcaps (GstBaseSrc * bsrc, GstCaps * filter)
{
  GstGMTPClientSrc *src;
  GstCaps *caps = NULL;

  src = GST_GMTP_CLIENT_SRC (bsrc);

  caps = (filter ? gst_caps_ref (filter) : gst_caps_new_any ());

  GST_DEBUG_OBJECT (src, "returning caps %" GST_PTR_FORMAT, caps);
  g_assert (GST_IS_CAPS (caps));
  return caps;
}

static gboolean
gst_gmtp_client_src_setcaps (GstBaseSrc * bsrc, GstCaps * caps)
{
  GstGMTPClientSrc *src;
  src = GST_GMTP_CLIENT_SRC (bsrc);
  GST_INFO("CHAMOUT SET CAPS DE CLIENT SRC");
  if ((caps != NULL) && (!gst_caps_is_equal (caps, GST_CAPS_ANY))) {
    if (!gst_pad_set_caps(bsrc->srcpad, caps)) {
       GST_ELEMENT_ERROR (src, CORE, NEGOTIATION, (NULL),
          ("Error setting caps."));
      return FALSE;
    }
  }
  return TRUE;
}

/*
 * Define the gst class, callbacks, etc.
 */
static void
gst_gmtp_client_src_class_init (GstGMTPClientSrcClass * klass)
{
  GObjectClass *gobject_class;
  GstBaseSrcClass *gstbasesrc_class;
  GstPushSrcClass *gstpush_src_class;
  GstElementClass *gstelement_class;

  gobject_class = (GObjectClass *) klass;
  gstbasesrc_class = (GstBaseSrcClass *) klass;
  gstpush_src_class = (GstPushSrcClass *) klass;
  gstelement_class = (GstElementClass *) klass;

  gobject_class->set_property = gst_gmtp_client_src_set_property;
  gobject_class->get_property = gst_gmtp_client_src_get_property;

  gobject_class->finalize = gst_gmtp_client_src_finalize;

  gst_element_class_add_pad_template (gstelement_class,
      gst_static_pad_template_get (&srctemplate));

  gst_element_class_set_static_metadata (gstelement_class, "GMTP client source",
      "Source/Network",
      "Receive data as a client over the network via GMTP",
      "E-Phone Team at Federal University of Campina Grande <leandroal@gmail.com>");

  g_object_class_install_property (G_OBJECT_CLASS (klass), PROP_PORT,
      g_param_spec_int ("port", "Port",
          "The port to receive packets from", 0, G_MAXUINT16,
          GMTP_DEFAULT_PORT, G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));

  g_object_class_install_property (gobject_class, PROP_HOST,
      g_param_spec_string ("host", "Host",
          "The host IP address to receive packets from", GMTP_DEFAULT_HOST,
          G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));

  g_object_class_install_property (gobject_class, PROP_SOCK_FD,
      g_param_spec_int ("sockfd", "Socket fd",
          "The socket file descriptor", -1, G_MAXINT, GMTP_DEFAULT_SOCK_FD,
          G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));

  g_object_class_install_property (gobject_class, PROP_CLOSED,
      g_param_spec_boolean ("close-socket", "Close socket",
          "Close socket at the end of stream", GMTP_DEFAULT_CLOSED,
          G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));

  g_object_class_install_property (gobject_class, PROP_CAPS,
      g_param_spec_boxed ("caps", "Caps",
          "The caps of the source pad", GST_TYPE_CAPS,
          G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));

  /*g_object_class_install_property (gobject_class, PROP_CCID,
      g_param_spec_int ("ccid", "CCID",
          "The Congestion Control IDentified to be used", 2, G_MAXINT,
          GMTP_DEFAULT_CCID, G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));*/

  /* signals */
  /**
   * GstDccpClientSrc::connected:
   * @src: the gstgmtpclientsrc element that emitted this signal
   * @fd: the connected socket file descriptor
   *
   * Reports that the element has connected, giving the fd of the socket
   */
  gst_gmtp_client_src_signals[SIGNAL_CONNECTED] =
      g_signal_new ("connected", G_TYPE_FROM_CLASS (klass), G_SIGNAL_RUN_FIRST,
      G_STRUCT_OFFSET (GstGMTPClientSrcClass, connected), NULL, NULL,
      g_cclosure_marshal_generic, G_TYPE_NONE, 1, G_TYPE_INT);

  gstbasesrc_class->start = gst_gmtp_client_src_start;
  gstbasesrc_class->stop = gst_gmtp_client_src_stop;
  gstpush_src_class->create = gst_gmtp_client_src_create;
  gstbasesrc_class->get_caps = gst_gmtp_client_src_getcaps;
  gstbasesrc_class->set_caps = gst_gmtp_client_src_setcaps;

  GST_DEBUG_CATEGORY_INIT (gmtpclientsrc_debug, "gmtpclientsrc", 0,
      "GMTP Client Source");
}
