#!/usr/bin/python

import sys
import socket
import time
import timeit

from datetime import datetime
from optparse import Option, OptionParser
from gmtp import *

default_port = 12345
default_if = 'eth1'
default_npkt = 1000

usage = "usage: %prog [options]. Note: -a takes precedence of -i, specify one or other."
parser = OptionParser(usage=usage, version="%prog 1.0")

parser.add_option("-i", "--iface", dest="iface", default=default_if,
                  help="The network interface to bind [default: %default].", metavar="IFACE")
parser.add_option("-a", "--address", dest="address",
                  help="The network address", metavar="ADDRESS")
parser.add_option("-p", "--port", dest="port", type="int",
                  help="The network port [default: %default]", default=default_port, metavar="PORT")
parser.add_option("-n", dest="npkt", type="int",
                  help="The number of packets to send [default: %default]", default=default_npkt, metavar="NPKT")

(options, args) = parser.parse_args()

if options.address:
    address = (options.address, options.port)
elif options.iface:
    ip = get_ip_address(options.iface)
    address = (ip, options.port)
else:
    parser.print_help()
    sys.exit(1)

npkt = default_npkt
if options.npkt:
    npkt = int(options.npkt)

msg = "Welcome to the jungle!"
out = "sair"

print "Starting server... at ", address
        
# Create sockets
server_socket = socket.socket(socket.AF_INET, socket.SOCK_GMTP, socket.IPPROTO_GMTP)
server_socket.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)

#flowname = "1234567812345678";
#server_socket.setsockopt(socket.SOL_GMTP, socket.GMTP_SOCKOPT_FLOWNAME, flowname)

#tx_rate = "max_gmtp_rate"
#tx_rate = 50000 #bytes/s.  250000 bytes/s == 2 Mbps
#server_socket.setsockopt(socket.SOL_GMTP, socket.GMTP_SOCKOPT_MAX_TX_RATE, tx_rate)


# Connect sockets
server_socket.bind(address)
threads = []
print "Listening for incoming connections...\n"
time.sleep(0.5)

server_socket.listen(4)
connection, client_addr = server_socket.accept()
print "[+] Client connected:", client_addr[0], ":", str(client_addr[1])

i = 1
total_size = 0
last_size = 0;
        
total_time = 0
start_time = 0
last_time = 0
 
#print "Sending text: '" + msg + "' at " +  str(tx_rate) + " bytes/s (max)\n"
print "Sending... "

try:
    while i <= npkt:
        
        #time.sleep(0.001); # App controls Tx    0,001 ~ 100.000 bytes/s
       
        if(i == 1):
            start_time = timeit.default_timer()
            last_time = timeit.default_timer()
        
        text = msg + " (" + str(i) + ")"
        size = getPacketSize(text)
        total_size = total_size + size
        
	try:
            connection.send(text.encode('utf-8'))
        except socket.error:
	    #connection.close()
            print "[+] Connection with client ", client_addr[0], ":", str(client_addr[1]), "closed."
            break

        i = i + 1
        
        if(i%50 == 0):
            sys.stdout.write("=>")
            sys.stdout.flush()
        
        if(i <= 10 or i%100 == 0):
            nowstr = str(datetime.now().strftime('%H:%M:%S:%f'))
            
            total_time = timeit.default_timer() - start_time
            elapsed = timeit.default_timer() - last_time
            size100 = total_size - last_size

            if(elapsed == 0 or total_time == 0): #avoid division by zero...
                 continue
            
            rate = "%.2f" % (size100/elapsed)
            total_rate = "%.2f" % (total_size/total_time)
            rate_mb = "%.2f" % (size100/elapsed/1000000)
            total_rate_mb = "%.2f" % (total_size/total_time/1000000)
            
            last_time = timeit.default_timer()
            last_size = total_size
            
            print "\nMessage", i, "sent to client at", nowstr +":\n", text
            print "\tPacket Size: ", size, "Bytes"
            print "\tSize of last 100:", size100, "Bytes / Time elapsed: ", elapsed, "s"
            print "\tTotal sent:", total_size, "Bytes / Total time:", total_time, "s"
            print "\tSend rate (last 100):", rate, "B/s | ", rate_mb, "MB/s"
            print "\tSend rate (total): ", total_rate, "B/s | ", total_rate_mb, "MB/s\n\n"
            print "Sending... "
            time.sleep(0.100) # 100 ms
        
except (KeyboardInterrupt, SystemExit):
    print '\nReceived keyboard interrupt, quitting...\n'
finally:
    print '\nShutting down server...\n'
    #connection.send(out.encode('utf-8'))
    connection.close()
    

