import numpy as np
from gnuradio import gr
import pmt
import struct

class lora_protocol_rx(gr.sync_block):
    def __init__(self):
        gr.sync_block.__init__(
            self,
            name='LoRaMultiPacket Receiver',
            in_sig=None,
            out_sig=None
        )
        self.message_port_register_in(pmt.intern('pdus_in'))
        self.set_msg_handler(pmt.intern('pdus_in'), self.handle_lora_rx)

        self.message_port_register_out(pmt.intern('app_out'))
        self.message_port_register_out(pmt.intern('pdus_out'))

        self.rx_buffer = {}

    def get_flags_string(self, flags):
        f = []
        if flags & 0x04: f.append("ACK_REQ")
        if flags & 0x08: f.append("SACK")
        if flags & 0x10: f.append("CONN_REQ")
        if flags & 0x20: f.append("CONN_ACK")
        if flags & 0x40: f.append("CONN_NACK")
        return "|".join(f) if f else "---"

    def handle_lora_rx(self, msg):
        try:
            if pmt.is_pair(msg):
                data_vector = pmt.cdr(msg)
                packet_bytes = bytes(pmt.u8vector_elements(data_vector))
            elif pmt.is_symbol(msg):
                packet_bytes = pmt.symbol_to_string(msg).encode('latin1')
            else:
                packet_bytes = bytes(pmt.u8vector_elements(msg))
        except Exception: return

        header_fmt = '<HBBBBB'
        header_size = 7
        if len(packet_bytes) < header_size: return

        msg_id, total_chunks, chunk_idx, payload_size, flags, version = struct.unpack(header_fmt,
packet_bytes[:header_size])

        if flags & 0x08: return

        print(f"[RX] ⬅ RICEZIONE DATI | MsgID: {msg_id} | Chunk: {chunk_idx+1:02d}/{total_chunks} | Payload: {payload_size:03d}B | Flags: [{self.get_flags_string(flags)}]")

        payload = packet_bytes[header_size : header_size + payload_size]

        if msg_id not in self.rx_buffer:
            self.rx_buffer[msg_id] = {}

        self.rx_buffer[msg_id][chunk_idx] = payload

        if flags & 0x04:
            self.send_sack(msg_id, total_chunks)

        if len(self.rx_buffer[msg_id]) == total_chunks:
            full_data = bytearray()
            for i in range(total_chunks):
                full_data += self.rx_buffer[msg_id][i]

            out_symbol = pmt.intern(f"RICEVUTO MsgID {msg_id} (COMPLETO!)")
            self.message_port_pub(pmt.intern('app_out'), out_symbol)
            import os
            m_file = os.environ.get('METRICS_FILE')
            if m_file:
                with open(m_file, 'a') as f:
                    f.write(f"[RX_METRICS] MsgID:{msg_id} Size:{len(full_data)} Retrans:0 Latency:0.0\n")
            else:
                print(f"[RX_METRICS] MsgID:{msg_id} Size:{len(full_data)} Retrans:0 Latency:0.0")
            del self.rx_buffer[msg_id]

    def send_sack(self, msg_id, total_chunks):
        bitmap_size = (total_chunks + 7) // 8
        bitmap = bytearray(bitmap_size)

        for i in range(total_chunks):
            if i in self.rx_buffer.get(msg_id, {}):
                byte_idx = i // 8
                bit_idx = i % 8
                bitmap[byte_idx] |= (1 << bit_idx)

        header = struct.pack('<HBBBBB', msg_id, total_chunks, 0, len(bitmap), 0x08, 1)
        packet_bytes = header + bitmap + b'\x00\x00'

        bitmap_str = "".join([bin(b)[2:].zfill(8)[::-1] for b in bitmap])[:total_chunks]
        print(f"[RX] ➔ INVIO SACK | MsgID: {msg_id} | Bitmap: {bitmap_str}")

        msg_symbol = pmt.intern(packet_bytes.decode('latin1'))
        self.message_port_pub(pmt.intern('pdus_out'), msg_symbol)
