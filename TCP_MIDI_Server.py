import socket
import ssl
import threading

# Load the server's private key and certificate
server_context = ssl.create_default_context(ssl.Purpose.CLIENT_AUTH)
server_context.load_cert_chain(certfile='/home/server_certificate.pem', keyfile='/home/server_private_key.pem')
# Create a secure socket and bind to port 8080
secure_sock = server_context.wrap_socket(socket.socket(socket.AF_INET, socket.SOCK_STREAM), server_side=True)
server_address = ('0.0.0.0', 8080)
print(f"Starting up on {server_address}")
secure_sock.bind(server_address)
secure_sock.listen()
print("Waiting to receive messages...")

# Global variable to keep track of unique client IDs
clients = {}
client_id_counter = 0
client_id_lock = threading.Lock()  # Lock for thread-safe access to the counter

def handle_client(secure_connection, client_address):
    global client_id_counter
    global clients

    with client_id_lock:
        client_id = client_id_counter + 1
        client_id_counter += 1

    client_ip, client_port = client_address
    print(f"Client connected: ID = {client_id}, IP = {client_ip}, Port = {client_port}")

    # Send the client ID to the connected client
    secure_connection.sendall(str(client_id).encode())

    # Store client in the clients dictionary
    clients[client_id] = {'connection': secure_connection}

    try:
        while True:
            # Receive data from the client
            data = secure_connection.recv(4096)  # Increased buffer size for RTP packets
            if not data:
                print(f"Client {client_id} disconnected.")
                break

            # Here you would decrypt the incoming RTP packet before processing it.
            # For now, we will just print it as a placeholder.
            print(f"Received message from Client {client_id}: {data}")

            # Route the message to all other clients
            for other_client_id, client_info in clients.items():
                if other_client_id != client_id:  # Filter out the sender
                    try:
                        client_info['connection'].sendall(data)  # Send raw data (encrypted RTP packet)
                    except Exception as e:
                        print(f"Error sending message to Client {other_client_id}: {e}")

    except Exception as e:
        print(f"Error handling client: {e}")

    finally:
        # Remove the client from the clients dictionary when disconnected
        with client_id_lock:
            del clients[client_id]
        secure_connection.close()
        print(f"Client disconnected: ID = {client_id}, IP = {client_ip}, Port = {client_port}")

while True:
    try:
        # Accept a secure connection
        secure_connection, client_address = secure_sock.accept()
        # Create a new thread for each client connection
        client_thread = threading.Thread(target=handle_client, args=(secure_connection, client_address))
        client_thread.start()
    except Exception as e:
        print(f"Error accepting new connection: {e}")