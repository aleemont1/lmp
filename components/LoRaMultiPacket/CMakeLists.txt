idf_component_register(SRCS "src/Packet.cpp" 
                            "src/PacketSerializer.cpp" 
                            "src/PacketValidator.cpp" 
                            "src/PacketParser.cpp" 
                            "src/PacketDeserializer.cpp" 
                            "src/PacketReassembler.cpp"
                            "src/LoRaProtocol.cpp"
                       INCLUDE_DIRS "include"
                       REQUIRES "RadioLib")