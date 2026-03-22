#pragma once

#include <array>
#include <map>
#include <vector>

#include "SPI.h"
#include "can_interface.h"
#include "mcp_can.h"

class MCP25XXCAN : public ICAN
{
public:
    enum class McpClockSpeed
    {
        k20Mhz = MCP_20MHZ,
        k16Mhz = MCP_16MHZ,
        k10Mhz = MCP_10MHZ,
        k8Mhz = MCP_8MHZ
    };
    /**
     * @brief Construct a new MCP25XXCAN object.
     *
     * @param cs The CAN CS pin
     * @param spi_bus The SPI bus to use (default SPI)
     * @param clock_speed The MCP clock speed
     */
    MCP25XXCAN(uint8_t rx_queue_size = 10,
               gpio_num_t cs = gpio_num_t::GPIO_NUM_15,
               SPIClass *spi_bus = &SPI,
               McpClockSpeed clock_speed = McpClockSpeed::k8Mhz)
        : mcp_can_{spi_bus, cs}, clock_speed_{clock_speed}
    {
    }
    // TODO: add support for interrupts for received messages?

    void Initialize(BaudRate baud) override
    {
        mcp_can_.begin(MCP_ANY, baud_rate_map_[baud], static_cast<INT8U>(clock_speed_));
        mcp_can_.setMode(MCP_NORMAL);
    }

    bool SendMessage(CANMessage &msg) override
    {
        byte send_result = mcp_can_.sendMsgBuf(msg.id_, msg.extended_id_ ? 1 : 0, msg.len_, msg.data_.data());
        return send_result == CAN_OK;
    }

    void RegisterRXMessage(ICANRXMessage &msg) override { rx_messages_.push_back(&msg); }

    void Tick() override
    {
        const uint8_t kMaxMessages = 3;
        uint8_t message_count = 0;
        static std::array<uint8_t, 8> msg_data{};
        static CANMessage received_message{0, 8, msg_data};
        while (mcp_can_.checkReceive() == CAN_MSGAVAIL && message_count <= kMaxMessages)
        {
            INT8U ext_int8u = 0;
            if (mcp_can_.readMsgBuf(reinterpret_cast<INT32U *>(&received_message.id_),
                                    &ext_int8u,
                                    reinterpret_cast<INT8U *>(&received_message.len_),
                                    reinterpret_cast<INT8U *>(received_message.data_.data()))
                == CAN_OK)
            {
                received_message.extended_id_ = ext_int8u;
                for (size_t i = 0; i < rx_messages_.size(); i++)
                {
                    rx_messages_[i]->DecodeSignals(received_message);
                }
            }
            else
            {
                printf("Failed to read message from CAN bus\n");
            }
            message_count++;
        }
    }

private:
    McpClockSpeed clock_speed_;
    MCP_CAN mcp_can_;
    std::vector<ICANRXMessage *> rx_messages_;

    std::map<ICAN::BaudRate, INT8U> baud_rate_map_{{ICAN::BaudRate::kBaud1M, CAN_1000KBPS},
                                                   {ICAN::BaudRate::kBaud500K, CAN_500KBPS},
                                                   {ICAN::BaudRate::kBaud250K, CAN_250KBPS},
                                                   {ICAN::BaudRate::kBaud125k, CAN_125KBPS}};
};