import socket
import struct
import mido
import ssl
import threading
from Cryptodome.Cipher import AES
from Cryptodome.Random import get_random_bytes
import time

class DummyPort:
    def __init__(self):
        self.name = 'DummyPort'
        self.closed = False

    def send(self, message):
        pass

    def receive(self):
        return None

    def close(self):
        self.closed = True

    def is_open(self):
        return not self.closed

# RTP packet format
RTP_VERSION = 2
RTP_PAYLOAD_TYPE = 96  # dynamic payload type
RTP_SEQ_NUM = 0
RTP_TIMESTAMP = 0
RTP_SSRC = 0x12345678

def create_rtp_packet(payload):
    packet = struct.pack('BBHII',
                        (RTP_VERSION << 6) | RTP_PAYLOAD_TYPE,
                        0,
                        RTP_SEQ_NUM,
                        RTP_TIMESTAMP,
                        RTP_SSRC)
    packet += struct.pack('!' + str(len(payload)) + 's', payload)
    return packet

def encrypt_rtp_packet(rtp_packet):
    key = get_random_bytes(16)  # Generate a random 16-byte key for AES encryption
    padding_length = 16 - (len(rtp_packet) % 16)
    rtp_packet += b'\x00' * padding_length  # Pad the RTP packet to a multiple of 16 bytes
    cipher = AES.new(key, AES.MODE_ECB)
    encrypted_bytes = cipher.encrypt(rtp_packet)
    return key + encrypted_bytes  # Prepend the encryption key to the encrypted RTP packet

def decrypt_rtp_packet(encrypted_rtp_packet):
    key = encrypted_rtp_packet[:16]  # Extract the encryption key from the beginning of the message
    ciphertext = encrypted_rtp_packet[16:]
    cipher = AES.new(key, AES.MODE_ECB)
    decrypted_rtp_packet = cipher.decrypt(ciphertext)
    decrypted_rtp_packet.rstrip(b'\x00')  # Remove any padding
    return decrypted_rtp_packet

# Create a SSL/TLS context
context = ssl.create_default_context()
context.check_hostname = False  
context.verify_mode = ssl.CERT_NONE  

# Bind socket to local address
server_addr = ('20.13.138.208', 8080)
#server_addr = ('localhost', 8080)  
secure_sock = context.wrap_socket(socket.socket(socket.AF_INET, socket.SOCK_STREAM))
secure_sock.connect(server_addr)
print(f"Connected to the server {server_addr}.")

# Receive the unique client ID from the server
client_id = secure_sock.recv(128).decode()
print(f"Connected to the server. Your unique client ID is: {client_id}")

def print_ports(heading, port_names):
    print(heading)
    index = 1
    for name in port_names:
        print(f" '{index} {name}'")
        index += 1
    print()

inputports = ['DummyPort'] + mido.get_input_names()
outputports = ['DummyPort'] + mido.get_output_names()

def print_ports(header, ports):
    print(header)
    for i, port in enumerate(ports, 1):
        print(f"{i}: {port}")

print_ports('Input Ports:', inputports)
print_ports('Output Ports:', outputports)

def select_inputport(MIDIinport):
    MIDIinput_selection = int(MIDIinport)
    selected_port = inputports[MIDIinput_selection - 1]
    print(f"You Selected the MIDI Input Port: '{selected_port}'")
    return DummyPort() if selected_port == 'DummyPort' else selected_port

def select_outputport(MIDIoutport):
    MIDIoutput_selection = int(MIDIoutport)
    selected_port = outputports[MIDIoutput_selection - 1]
    print(f"You Selected the MIDI Output Port: '{selected_port}'")
    return DummyPort() if selected_port == 'DummyPort' else selected_port

print("Select the MIDI Input Port: ")
MIDIinport = input()
MIDI_inPortName = select_inputport(MIDIinport)

print("Select the MIDI Output Port: ")
MIDIoutport = input()
MIDI_outPortName = select_outputport(MIDIoutport)

def send_messages():
    while True:
        try:
            with mido.open_input(MIDI_inPortName) as port:
                print(f'Using {port}')
                print('Waiting for messages...')
                for message in port:
                    print(f"Sending: {message}")
                    rtp_packet = create_rtp_packet(message.bin())
                    encrypted_rtp_packet = encrypt_rtp_packet(rtp_packet)
                    # Send encrypted RTP packet to server
                    secure_sock.sendall(encrypted_rtp_packet)                    # Optional: Implement a keep-alive mechanism
                    #time.sleep(0.1)  # Adjust as necessary for your application
                    
        except Exception as e:
            print(f"Error sending message: {e}")
            break

def receive_messages(sock):
    while True:
        try:
            response = sock.recv(128)
            if response:
                decrypted_rtp_packet = decrypt_rtp_packet(response)
                midi_message = decrypted_rtp_packet[12:]  # Extract MIDI message from RTP packet
                midi_messages = mido.parse_all(midi_message)
                with mido.open_output(MIDI_outPortName, autoreset=True) as port:
                    for msg in midi_messages:
                        port.send(msg)  # Send each valid MIDI message to the output port
                        print(f"Received: {msg}")
            else:
                print("No data received from the server")
                break
                
        except Exception as e:
            print(f"Error receiving message: {e}")
            break

# Start sending and receiving threads
send_thread = threading.Thread(target=send_messages)
send_thread.daemon = True
send_thread.start()

receive_thread = threading.Thread(target=receive_messages, args=(secure_sock,))
receive_thread.daemon = True
receive_thread.start()

# Keep the main thread alive while threads run
try:
    while True:
        pass  # Keep running until interrupted
except KeyboardInterrupt:
    print("Exiting...")

# Close the secure connection at exit
secure_sock.close()
