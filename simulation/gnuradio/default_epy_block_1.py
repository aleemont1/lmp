import numpy as np
from gnuradio import gr
import pmt
import struct

class lora_protocol_tx(gr.sync_block):
    def __init__(self):
        gr.sync_block.__init__(
            self,
            name='LoRaMultiPacket Sender',
            in_sig=None,
            out_sig=None
        )
        self.message_port_register_out(pmt.intern('pdus_out'))
        self.message_port_register_in(pmt.intern('app_in'))
        self.set_msg_handler(pmt.intern('app_in'), self.handle_app_msg)

        self.msg_id_counter = 1
        self.MAX_PAYLOAD = 246  # 255 (max LoRa) - 7 (header) - 2 (CRC) = 246 byte

    def handle_app_msg(self, msg):
        # GNURadio usa i PMT (Polymorphic Types). Estraiamo i dati grezzi dal messaggio in ingresso
        if pmt.is_pair(msg):
            data_vector = pmt.cdr(msg)
            raw_data = bytes(pmt.u8vector_elements(data_vector))
        else:
            raw_data = b"Test String for LoRaMultiPacket GNURadio Simulation!"

        total_chunks = (len(raw_data) + self.MAX_PAYLOAD - 1) // self.MAX_PAYLOAD

        # Frammentazione! (Il cuore del nostro protocollo)
        for i in range(total_chunks):
            start_idx = i * self.MAX_PAYLOAD
            end_idx = min(start_idx + self.MAX_PAYLOAD, len(raw_data))
            chunk_data = raw_data[start_idx:end_idx]

            # Impostiamo i flag (ACK_REQ solo sull'ultimo chunk)
            flags = 0
            if i == total_chunks - 1:
                flags |= 0x04  # FLAG_ACK_REQ

            # Creiamo l'Header di 7 byte identico al C++ usando struct
            # <HBBBBB significa: little-endian, uint16, uint8, uint8, uint8, uint8, uint8
            header = struct.pack('<HBBBBB',
                                    self.msg_id_counter,
                                    total_chunks,
                                    i,
                                    len(chunk_data),
                                    flags,
                                    1) # version

            # Qui andrebbe il calcolo del Modbus CRC-16 (mettiamo 2 byte dummy per ora per la simulazione)
            crc_dummy = b'\x00\x00'

            # Assembliamo il pacchetto OTA completo!
            packet_bytes = header + chunk_data + crc_dummy

            # Lo convertiamo nel formato PDU che il LoRa TX si aspetta e lo spariamo fuori!
            pdu = pmt.cons(pmt.make_dict(), pmt.init_u8vector(len(packet_bytes), list(packet_bytes)))
            self.message_port_pub(pmt.intern('pdus_out'), pdu)

        self.msg_id_counter += 1
