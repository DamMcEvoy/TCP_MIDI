import socket
import struct
import mido
import ssl
import threading
from Cryptodome.PublicKey import RSA
from Cryptodome.Cipher import PKCS1_v1_5, AES
from Cryptodome.Random import get_random_bytes

# RTP packet format
RTP_VERSION = 2
RTP_PAYLOAD_TYPE = 96  # dynamic payload type
RTP_SEQ_NUM = 0
RTP_TIMESTAMP = 0
RTP_SSRC = 0x12345678


# Create a SSL/TLS context
context = ssl.create_default_context()
context.check_hostname = False
context.verify_mode = ssl.CERT_NONE

# Bind socket to local address
server_addr = ('localhost', 8080)
secure_sock = context.wrap_socket(socket.socket(socket.AF_INET, socket.SOCK_STREAM))
secure_sock.connect(server_addr)
print(f"Connected to the server {server_addr}.")

# Échange des clés publique asymétriques
private_key_rsa = RSA.generate(2048)
public_key_rsa = private_key_rsa.publickey()
aes_key = get_random_bytes(16)

key_rsa = public_key_rsa.exportKey('PEM')
secure_sock.sendall(key_rsa)


def create_rtp_packet(payload):
    global RTP_SEQ_NUM, RTP_TIMESTAMP
    packet = struct.pack('BBHII',
                        (RTP_VERSION << 6) | RTP_PAYLOAD_TYPE,
                        0,
                        RTP_SEQ_NUM,
                        RTP_TIMESTAMP,
                        RTP_SSRC)
    packet += struct.pack('!' + str(len(payload)) + 's', payload)
    RTP_SEQ_NUM += 1
    return packet


def encrypt_rtp_packet(rtp_packet):
    global packet_length
    padding_length = 16 - (len(rtp_packet) % 16)
    rtp_packet += b'\x00' * padding_length  # Pad the RTP packet to a multiple of 16 bytes
    cipher = AES.new(aes_key, AES.MODE_ECB)
    encrypted_bytes = cipher.encrypt(rtp_packet)
    packet_length = len(encrypted_bytes)
    return encrypted_bytes  # Prepend the encryption key to the encrypted RTP packet


def decrypt_rtp_packet(encrypted_rtp_packet):

    # On déchiffre la clé publique AES de l'autre client avec notre clé privée RSA

    cipher_rsa = PKCS1_v1_5.new(private_key_rsa)
    decrypted_other_aes_key = cipher_rsa.decrypt(other_encrypted_aes_key, sentinel=None)
    # print(f"Clé déchiffré AES de l'autre client :  {decrypted_other_aes_key}")

    # On déchiffre le paquet RTP avec la clé publique AES de l'autre client qui à été déchiffré
    cipher_aes = AES.new(decrypted_other_aes_key, AES.MODE_ECB)
    decrypted_rtp_packet = cipher_aes.decrypt(encrypted_rtp_packet)
    return decrypted_rtp_packet.strip(b'\x00')  # Remove any padding


# Receive the unique client ID from the server
client_id = secure_sock.recv(1024).decode()
print(f"Connected to the server. Your unique client ID is: {client_id}")


def print_ports(heading, port_names):
    print(heading)
    index = 1
    for name in port_names:
        print(f" '{index} {name}'")
        index += 1
    print()


inputports = mido.get_input_names()
outputports = mido.get_output_names()
print_ports('Input Ports:', inputports)
print_ports('Output Ports:', outputports)


def select_inputport(MIDIinport):
    MIDIinput_selection = int(MIDIinport)
    print(f"You Selected the MIDI Input Port: '{inputports[(MIDIinput_selection)-1]}'")
    return inputports[(MIDIinput_selection)-1]


def select_outputport(MIDIoutport):
    MIDIoutput_selection = int(MIDIoutport)
    print(f"You Selected the MIDI Output Port: '{outputports[(MIDIoutput_selection)-1]}'")
    return outputports[(MIDIoutput_selection)-1]


print("Select the MIDI Input Port: ")
MIDIinport = input()
MIDI_inPortName = select_inputport(MIDIinport)
print("Select the MIDI Output Port: ")
MIDIoutport = input()
MIDI_outPortName = select_outputport(MIDIoutport)


def rsa_key_exchange():
    try:
        global encrypted_other_aes_key
        other_public_key = secure_sock.recv(4096)
        other_client_public_key = RSA.import_key(other_public_key)
        cipher = PKCS1_v1_5.new(other_client_public_key)
        encrypted_other_aes_key = cipher.encrypt(aes_key)
        secure_sock.sendall(encrypted_other_aes_key)
    except Exception as e:
        print(f"Error exchanging rsa keys {e}")


def send_messages():
    while True:
        try:
            with mido.open_input(MIDI_inPortName) as port:
                print(f'Using {port}')
                print('Waiting for messages...')
                for message in port:
                    print(f"Sending: {message.bin()}")
                    rtp_packet = create_rtp_packet(message.bin())
                    encrypted_rtp_packet = encrypt_rtp_packet(rtp_packet)

                    secure_sock.sendall(encrypted_rtp_packet)  # Send encrypted RTP packet to server
                    # Optional: Implement a keep-alive mechanism
                    # time.sleep(0.1)  # Adjust as necessary for your application
                    # secure_sock.sendall(private_key)
        except Exception as e:
            print(f"Error sending message: {e}")
            break


def receive_messages(sock):
    global other_encrypted_aes_key
    packet_length = 1024
    keyRound = True
    while True:
        try:
            if keyRound:
                other_encrypted_aes_key = sock.recv(4096)
                print(other_encrypted_aes_key)
                keyRound = False
            response = sock.recv(packet_length)
            if response:
                midi_message = decrypt_rtp_packet(response)  # Extract MIDI message from RTP packet
                print(f"Message MIDI déchiffré  : {midi_message}")
                midi_messages = mido.parse_all(midi_message)
                with mido.open_output(MIDI_outPortName, autoreset=True) as port:
                    for msg in midi_messages:
                        port.send(msg)  # Send each valid MIDI message to the output port
                        print(f"Received: {msg.bin()}")
            else:
                print("No data received from the server")
                break
        except Exception as e:
            print(f"Error receiving message: {e}")
            break


# Start sending and receiving threads

rsa_key_thread = threading.Thread(target=rsa_key_exchange)
rsa_key_thread.daemon = True
rsa_key_thread.start()

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
