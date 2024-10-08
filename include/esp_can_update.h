#pragma once

#ifdef ARDUINO_ARCH_ESP32

#include <Arduino.h>
#include <Update.h>

#include <array>

#include "CAN.h"

class CANUpdate
{
public:
    CANUpdate(uint32_t update_id, ICAN &can_bus, VirtualTimerGroup &timer_group)
        : kUpdateId{update_id}, can_interface_{can_bus}, timer_group_{timer_group}
    {
        fw_version_ = kFirmwareVersion;
        update_data_message_.SetMask(0x7FF);
        timer_group_.AddTimer(100,
                              [this]()
                              {
                                  if (update_data_message_.GetTimeSinceLastReceive() >= kUpdateTimeout
                                      && update_info_message_.GetTimeSinceLastReceive() >= kUpdateTimeout)
                                  {
                                      update_started_ = false;
                                      received_md5_ = false;
                                      received_len_ = false;
                                      written_ = false;
                                      received_md5_arr_.at(0) = false;
                                      received_md5_arr_.at(1) = false;
                                      received_md5_arr_.at(2) = false;
                                      received_md5_arr_.at(3) = false;
                                      update_block_idx_ = 0;
                                      Update.abort();
                                  }
                              });
    }

private:
    const uint32_t kUpdateId;
    ICAN &can_interface_;
    VirtualTimerGroup &timer_group_;

#ifdef UNIX_TIMESTAMP
    const uint32_t kFirmwareVersion{UNIX_TIMESTAMP};
#else
    constexpr uint32_t kFirmwareVersion{0};
#endif

    const uint32_t kUpdateTimeout{500};

    enum class MessageType : uint8_t
    {
        kUpdateStart = 0,
        kMd5 = 1
    };
    MakeUnsignedCANSignal(MessageType, 0, 8, 1, 0) message_type_{};
    MakeUnsignedCANSignal(uint32_t, 8, 32, 1, 0) update_length_{};
    MakeUnsignedCANSignal(uint16_t, 8, 8, 1, 0) update_md5_idx_{};
    MakeUnsignedCANSignal(uint32_t, 16, 32, 1, 0) update_md5_{};

    MultiplexedSignalGroup<1, MessageType> length_signal_group_{MessageType::kUpdateStart, update_length_};
    MultiplexedSignalGroup<2, MessageType> md5_signal_group_{MessageType::kMd5, update_md5_idx_, update_md5_};

    MakeUnsignedCANSignal(uint8_t, 0, 8, 1, 0) data_block_index_low_{};
    MakeUnsignedCANSignal(uint64_t, 8, 56, 1, 0) update_data_{};

    MakeUnsignedCANSignal(uint32_t, 0, 24, 1, 0) update_block_idx_{};
    MakeUnsignedCANSignal(bool, 24, 1, 1, 0) received_len_{};
    MakeUnsignedCANSignal(bool, 25, 1, 1, 0) received_md5_{};
    MakeUnsignedCANSignal(bool, 26, 1, 1, 0) written_{};
    MakeUnsignedCANSignal(uint32_t, 32, 32, 1, 0) fw_version_{};

    CANTXMessage<5> update_progress_message_{can_interface_,
                                             kUpdateId + 2,
                                             8,
                                             100,
                                             timer_group_,
                                             update_block_idx_,
                                             received_len_,
                                             received_md5_,
                                             written_,
                                             fw_version_};

    bool update_started_ = false;
    std::array<bool, 4> received_md5_arr_ = {false, false, false, false};
    std::array<uint32_t, 4> md5_arr_{};
    char md5_cstr_[33];

    MultiplexedCANRXMessage<2, MessageType> update_info_message_{
        can_interface_,
        kUpdateId + 1,
        [this]()
        {
            if (message_type_ == MessageType::kMd5)
            {
                received_md5_arr_.at(static_cast<uint8_t>(update_md5_idx_)) = true;
                md5_arr_[static_cast<uint8_t>(update_md5_idx_)] = update_md5_;
                if (received_md5_arr_.at(0) && received_md5_arr_.at(1) && received_md5_arr_.at(2)
                    && received_md5_arr_.at(3))
                {
                    received_md5_ = true;
                }
            }
            else if (!update_started_ && message_type_ == MessageType::kUpdateStart)
            {
                sprintf(md5_cstr_,
                        "%08x%08x%08x%08x",
                        __bswap32(md5_arr_.at(0)),
                        __bswap32(md5_arr_.at(1)),
                        __bswap32(md5_arr_.at(2)),
                        __bswap32(md5_arr_.at(3)));
                Serial.printf("MD5: %s\n", md5_cstr_);
                Serial.printf("Length: %u\n", static_cast<uint32_t>(update_length_));
                if (!Update.begin(update_length_))
                {
                    Update.printError(Serial);
                }
                else
                {
                    Update.setMD5(md5_cstr_);
                    update_block_idx_ = 0;
                    received_len_ = true;
                    update_started_ = true;
                    written_ = false;
                    update_progress_message_.EncodeAndSend();
                }
            }
        },
        message_type_,
        length_signal_group_,
        md5_signal_group_};

    CANRXMessage<2> update_data_message_{can_interface_,
                                         kUpdateId,
                                         [this]()
                                         {
                                             uint64_t data = update_data_;
                                             uint32_t index = ((update_data_message_.GetID() & 0x1FFFF800/*18 bits of CAN extended ID, not including standard ID*/) >> 3) + data_block_index_low_;
                                             if (update_started_ && static_cast<uint32_t>(update_block_idx_) == index)
                                             {
                                                 if (index * 7 >= update_length_ - 7)
                                                 {
                                                     Update.write(reinterpret_cast<uint8_t *>(&data),
                                                                  update_length_ - (index * 7));
                                                     written_ = true;
                                                     update_progress_message_.EncodeAndSend();

                                                     if (Update.end())
                                                     {
                                                         Serial.println("Update success!");
                                                         ESP.restart();
                                                     }
                                                     else
                                                     {
                                                         Update.printError(Serial);
                                                         Serial.printf("Expected MD5: %s\n", md5_cstr_);
                                                         update_started_ = false;
                                                     }
                                                 }
                                                 else
                                                 {
                                                     Update.write(reinterpret_cast<uint8_t *>(&data), 7);
                                                     written_ = true;
                                                     update_progress_message_.EncodeAndSend();
                                                     update_block_idx_ += 1;
                                                     written_ = false;
                                                     update_progress_message_.EncodeAndSend();
                                                 }
                                             }
                                         },
                                         data_block_index_low_,
                                         update_data_};
};
#endif
