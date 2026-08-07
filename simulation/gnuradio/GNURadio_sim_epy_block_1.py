import numpy as np
from gnuradio import gr
import pmt
import struct
import threading
import time

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

        self.message_port_register_in(pmt.intern('pdus_in'))
        self.set_msg_handler(pmt.intern('pdus_in'), self.handle_sack)

        self.msg_id_counter = 1
        self.MAX_PAYLOAD = 200
        self.tx_buffer = {}
        
        self.time_module = time
        self.start_times = {}
        self.retransmission_counts = {}
        self.total_chunks_sent = {}
        
        # Threading for ARQ timeout
        self.lock = threading.Lock()
        self.last_tx_time = {}
        self.retries = {}
        self.MAX_RETRIES = 5
        # Timeout calibrato a 1.5s per resistere ai ritardi di scheduling CPU 
        # quando si lanciano 12 thread paralleli (SF10 richiede molta CPU)
        self.ACK_TIMEOUT = 1.5 
        
        self.completed_messages = 0
        
        # Innesco automatico del primo messaggio (disaccoppia completamente dallo strobe)
        threading.Timer(0.2, self.handle_app_msg, args=(None,)).start()
        
        self.monitor_thread = threading.Thread(target=self.monitor_timeouts, daemon=True)
        self.monitor_thread.start()

    def _write_metrics(self, line: str):
        """Scrive una riga [METRICS] sul file specificato da METRICS_FILE."""
        import os
        path = os.environ.get('METRICS_FILE', '')
        if path:
            with open(path, 'a') as f:
                f.write(line + '\n')
                f.flush()

    def get_flags_string(self, flags):
        f = []
        if flags & 0x04: f.append("ACK_REQ")
        if flags & 0x08: f.append("SACK")
        if flags & 0x10: f.append("CONN_REQ")
        if flags & 0x20: f.append("CONN_ACK")
        if flags & 0x40: f.append("CONN_NACK")
        return "|".join(f) if f else "---"

    def monitor_timeouts(self):
        import os
        if os.environ.get('UNRELIABLE_MODE') == '1': return
        while True:
            self.time_module.sleep(0.02)
            with self.lock:
                current_time = self.time_module.time()
                for msg_id in list(self.tx_buffer.keys()):
                    if current_time - self.last_tx_time.get(msg_id, current_time) > self.ACK_TIMEOUT:
                        if self.retries.get(msg_id, 0) < self.MAX_RETRIES:
                            self.retries[msg_id] = self.retries.get(msg_id, 0) + 1
                            self.last_tx_time[msg_id] = current_time
                            self.retransmission_counts[msg_id] += 1
                            
                            print(f"[TX] ⚠ ACK TIMEOUT! Ritrasmissione ultimo chunk (ACK_REQ) per MsgID: {msg_id} (Tentativo {self.retries[msg_id]}/{self.MAX_RETRIES})")
                            
                            # Prendi l'ultimo chunk dal buffer
                            pkt = bytearray(self.tx_buffer[msg_id][-1])
                            pkt[5] |= 0x04 # Set ACK_REQ flag (bit 2)
                            
                            flags_str = self.get_flags_string(pkt[5])
                            print(f"[TX] ➔ RETRY PACKET | MsgID: {msg_id} | Flags: [{flags_str}]")
                            
                            self.message_port_pub(pmt.intern('pdus_out'), pmt.intern(pkt.decode('latin1')))
                        else:
                            print(f"[TX] ❌ MSGID {msg_id} FALLITO. Max retries superati.")
                            del self.tx_buffer[msg_id]
                            self._check_done()

    def _check_done(self):
        self.completed_messages += 1
        if self.completed_messages < 3:
            threading.Timer(0.01, self.handle_app_msg, args=(None,)).start()
        else:
            import os
            m_file = os.environ.get('METRICS_FILE', '')
            if m_file and os.environ.get('UNRELIABLE_MODE') == '1':
                # Wait until the count of [RX_METRICS] lines stabilises for 1s
                # (no new arrivals in the last 1s) OR 10s absolute timeout.
                # - All delivered: stabilises after last msg written by RX (~ms after TX done)
                # - All dropped  : immediately stable at 0 → exits after 1s
                abs_deadline = self.time_module.time() + 10.0
                last_count = -1
                stable_since = self.time_module.time()
                while self.time_module.time() < abs_deadline:
                    try:
                        with open(m_file, 'r') as _f:
                            n = sum(1 for l in _f if '[RX_METRICS]' in l)
                    except Exception:
                        n = last_count
                    if n != last_count:
                        last_count = n
                        stable_since = self.time_module.time()
                    elif self.time_module.time() - stable_since >= 1.0:
                        break  # stable for 1s → done
                    if n >= 3:
                        break  # all 3 arrived → done immediately
                    self.time_module.sleep(0.05)
                self.time_module.sleep(0.1)  # tiny final flush buffer
            else:
                self.time_module.sleep(5.0)
            os._exit(0)


    def handle_app_msg(self, msg):
        frase = "[LORA_PAYLOAD_TEST] Questo è un payload gigante generato per forzare la frammentazione del protocollo alla sua massima capacità nominale (246 byte per chunk). Continuiamo ad aggiungere testo inutile per saturare la banda radio e far sudare il SX1262... "
        testo_gigante = frase * 15
        raw_data = testo_gigante.encode('utf-8')

        total_chunks = (len(raw_data) + self.MAX_PAYLOAD - 1) // self.MAX_PAYLOAD
        msg_id = self.msg_id_counter
        self.msg_id_counter += 1

        with self.lock:
            self.tx_buffer[msg_id] = []
            self.start_times[msg_id] = self.time_module.time()
            self.retransmission_counts[msg_id] = 0
            self.total_chunks_sent[msg_id] = total_chunks
            self.last_tx_time[msg_id] = self.time_module.time()
            self.retries[msg_id] = 0
            
        print(f"\n=======================================================")
        print(f"[TX] INIZIO MsgID {msg_id} ({len(raw_data)}B in {total_chunks} chunk)")
        print(f"=======================================================")

        for i in range(total_chunks):
            start_idx = i * self.MAX_PAYLOAD
            end_idx = min(start_idx + self.MAX_PAYLOAD, len(raw_data))
            chunk_data = raw_data[start_idx:end_idx]

            flags = 0
            if i == total_chunks - 1:
                import os
                if os.environ.get('UNRELIABLE_MODE') != '1':
                    flags |= 0x04

            header = struct.pack('<HBBBBB', msg_id, total_chunks, i, len(chunk_data), flags, 1)
            packet_bytes = header + chunk_data + b'\x00\x00'

            with self.lock:
                self.tx_buffer[msg_id].append(packet_bytes)

            print(f"[TX] ➔ INVIO DATI | MsgID: {msg_id} | Chunk: {i+1:02d}/{total_chunks} | Payload: {len(chunk_data):03d}B | Flags: [{self.get_flags_string(flags)}]")
            self.message_port_pub(pmt.intern('pdus_out'), pmt.intern(packet_bytes.decode('latin1')))

        # In Unreliable mode: emit METRICS right after last chunk (no ACK to wait for)
        import os
        if os.environ.get('UNRELIABLE_MODE') == '1':
            latency = self.time_module.time() - self.start_times[msg_id]
            self._write_metrics(f"[METRICS] MsgID:{msg_id} Size:{total_chunks} Retrans:0 Latency:{latency:.3f}")
            with self.lock:
                self.tx_buffer.pop(msg_id, None)
            self._check_done()

    def handle_sack(self, msg):
        try:
            if pmt.is_pair(msg):
                packet_bytes = bytes(pmt.u8vector_elements(pmt.cdr(msg)))
            elif pmt.is_symbol(msg):
                packet_bytes = pmt.symbol_to_string(msg).encode('latin1')
            else:
                return
        except Exception: return

        if len(packet_bytes) < 7: return
        msg_id, total_chunks, chunk_idx, payload_size, flags, version = struct.unpack('<HBBBBB', packet_bytes[:7])

        if not (flags & 0x08): return
        
        with self.lock:
            if msg_id not in self.tx_buffer: return
            
            # Reset timeout retries upon receiving a valid SACK
            self.retries[msg_id] = 0
            self.last_tx_time[msg_id] = self.time_module.time()

            bitmap = packet_bytes[7 : 7 + payload_size]

            # Generiamo la stringa visiva del bitmap (es. '11110111...')
            bitmap_str = "".join([bin(b)[2:].zfill(8)[::-1] for b in bitmap])[:total_chunks]
            print(f"[TX] ⬅ RICEZIONE SACK | MsgID: {msg_id} | Bitmap: {bitmap_str}")

            missing_indices = []
            for i in range(total_chunks):
                if bitmap_str[i] == '0':
                    missing_indices.append(i)

            if len(missing_indices) == 0:
                print(f"[TX] ✔ TRASMISSIONE COMPLETATA CON SUCCESSO (MsgID {msg_id})")
                
                latency = self.time_module.time() - self.start_times[msg_id]
                retrans = self.retransmission_counts[msg_id]
                chunks  = self.total_chunks_sent[msg_id]
                self._write_metrics(f"[METRICS] MsgID:{msg_id} Size:{chunks} Retrans:{retrans} Latency:{latency:.3f}")
                
                del self.tx_buffer[msg_id]
                self._check_done()
            else:
                print(f"[TX] ⚠ PACCHETTI PERSI: {missing_indices}. Avvio Ritrasmissione...")
                for idx, missing_i in enumerate(missing_indices):
                    if missing_i >= len(self.tx_buffer[msg_id]): continue
                    pkt = bytearray(self.tx_buffer[msg_id][missing_i])

                    if idx == len(missing_indices) - 1:
                        pkt[5] |= 0x04
                    else:
                        pkt[5] &= ~0x04

                    self.retransmission_counts[msg_id] += 1
                    flags_str = self.get_flags_string(pkt[5])
                    print(f"[TX] ➔ RITRASMISSIONE | MsgID: {msg_id} | Chunk: {missing_i+1:02d}/{total_chunks} | Flags: [{flags_str}]")
                    self.message_port_pub(pmt.intern('pdus_out'), pmt.intern(pkt.decode('latin1')))
