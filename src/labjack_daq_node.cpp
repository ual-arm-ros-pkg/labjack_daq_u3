/*---------------------------------------------------------------------------
 *  Labjack DAQ USB devices ROS 2 node
 *  Copyright, José Luis Blanco-Claraco, University of Almería (C) 2023
 *  License: MIT
 *-------------------------------------------------------------------------- */

#include <cstdint>
#include <memory>
#include <rclcpp/rclcpp.hpp>
#include <std_msgs/msg/float32_multi_array.hpp>
#include <vector>

#include "u3.h"

int ConfigIO_example(HANDLE hDevice, int* isDAC1Enabled);
int StreamConfig_example(HANDLE hDevice);
int StreamStart(HANDLE hDevice);
int StreamStop(HANDLE hDevice);

// For this example to work proper SamplesPerPacket needs to be a multiple of
// NumChannels.
constexpr uint8 NumChannels = 5;

// Needs to be 25 to read multiple  StreamData responses in one large packet,
// otherwise can be any value between 1-25 for 1 StreamData response per packet.
constexpr uint8 SamplesPerPacket = 25;

class LabjackNode : public rclcpp::Node
{
   public:
    LabjackNode() : Node("labjack_daq")
    {
        // Open the device:
        // Opening first found U3 over USB
        if ((hDevice_ = openUSBConnection(-1)) == nullptr)
            throw std::runtime_error("Error: openUSBConnection");

        // Getting calibration information from U3
        if (getCalibrationInfo(hDevice_, &caliInfo_) < 0)
            throw std::runtime_error("Error: getCalibrationInfo");

        if (ConfigIO_example(hDevice_, &dac1Enabled_) != 0)
            throw std::runtime_error("Error: ConfigIO_example");

        // Stopping any previous streams
        StreamStop(hDevice_);

        if (StreamConfig_example(hDevice_) != 0)
            throw std::runtime_error("Error: StreamConfig_example");

        if (StreamStart(hDevice_) != 0)
            throw std::runtime_error("Error: StreamStart");

        // Parameters
        this->declare_parameter<double>("publish_rate", publish_rate_);
        this->get_parameter("publish_rate", publish_rate_);

        timerPub_ = this->create_wall_timer(
            std::chrono::duration<double>(1.0 / publish_rate_),
            std::bind(&LabjackNode::onReadAndPubTimer, this));

        adcPub_ = this->create_publisher<std_msgs::msg::Float32MultiArray>(
            "gpio_adc", 10);
    }

    ~LabjackNode()
    {
        StreamStop(hDevice_);
        closeUSBConnection(hDevice_);
    }

   private:
    double                       publish_rate_ = 50.0;
    rclcpp::TimerBase::SharedPtr timerPub_;
    rclcpp::Publisher<std_msgs::msg::Float32MultiArray>::SharedPtr adcPub_;

    HANDLE            hDevice_ = nullptr;
    u3CalibrationInfo caliInfo_;
    int               dac1Enabled_;

    void onReadAndPubTimer();
};

int main(int argc, char** argv)
{
    rclcpp::init(argc, argv);
    auto node = std::make_shared<LabjackNode>();
    rclcpp::spin(node);
    rclcpp::shutdown();
    return 0;
}

// Sends a ConfigIO low-level command that configures the FIOs, DAC, Timers and
// Counters for this example
int ConfigIO_example(HANDLE hDevice, int* isDAC1Enabled)
{
    printf("ConfigIO_example...\n");

    uint8  sendBuff[12], recBuff[12];
    uint16 checksumTotal;
    int    sendChars, recChars;

    sendBuff[1] = (uint8)(0xF8);  // Command byte
    sendBuff[2] = (uint8)(0x03);  // Number of data words
    sendBuff[3] = (uint8)(0x0B);  // Extended command number

    sendBuff[6] =
        13;  // Writemask : Setting writemask for timerCounterConfig (bit 0),
             //            FIOAnalog (bit 2) and EIOAnalog (bit 3)

    sendBuff[7] = 0;  // Reserved
    sendBuff[8] =
        64;  // TimerCounterConfig: Disabling all timers and counters,
             //                    set TimerCounterPinOffset to 4 (bits 4-7)
    sendBuff[9] = 0;  // DAC1Enable

    sendBuff[10] = 255;  // FIOAnalog : setting all FIOs as analog inputs
    sendBuff[11] = 255;  // EIOAnalog : setting all EIOs as analog inputs
    extendedChecksum(sendBuff, 12);

    // Sending command to U3
    if ((sendChars = LJUSB_Write(hDevice, sendBuff, 12)) < 12)
    {
        if (sendChars == 0)
            printf("ConfigIO error : write failed\n");
        else
            printf("ConfigIO error : did not write all of the buffer\n");
        return -1;
    }

    // Reading response from U3
    if ((recChars = LJUSB_Read(hDevice, recBuff, 12)) < 12)
    {
        if (recChars == 0)
            printf("ConfigIO error : read failed\n");
        else
            printf("ConfigIO error : did not read all of the buffer\n");
        return -1;
    }

    checksumTotal = extendedChecksum16(recBuff, 12);
    if ((uint8)((checksumTotal / 256) & 0xFF) != recBuff[5])
    {
        printf("ConfigIO error : read buffer has bad checksum16(MSB)\n");
        return -1;
    }

    if ((uint8)(checksumTotal & 0xFF) != recBuff[4])
    {
        printf("ConfigIO error : read buffer has bad checksum16(LBS)\n");
        return -1;
    }

    if (extendedChecksum8(recBuff) != recBuff[0])
    {
        printf("ConfigIO error : read buffer has bad checksum8\n");
        return -1;
    }

    if (recBuff[1] != (uint8)(0xF8) || recBuff[2] != (uint8)(0x03) ||
        recBuff[3] != (uint8)(0x0B))
    {
        printf("ConfigIO error : read buffer has wrong command bytes\n");
        return -1;
    }

    if (recBuff[6] != 0)
    {
        printf(
            "ConfigIO error : read buffer received errorcode %d\n", recBuff[6]);
        return -1;
    }

    if (recBuff[8] != 64)
    {
        printf(
            "ConfigIO error : TimerCounterConfig did not get set correctly\n");
        return -1;
    }

    if (recBuff[10] != 255 && recBuff[10] != (uint8)(0x0F))
    {
        printf("ConfigIO error : FIOAnalog did not set get correctly\n");
        return -1;
    }

    if (recBuff[11] != 255)
    {
        printf(
            "ConfigIO error : EIOAnalog did not set get correctly (%d)\n",
            recBuff[11]);
        return -1;
    }

    *isDAC1Enabled = (int)recBuff[9];

    printf("ConfigIO_example... OK\n");
    return 0;
}

// Sends a StreamConfig low-level command to configure the stream.
int StreamConfig_example(HANDLE hDevice)
{
    uint8  sendBuff[64], recBuff[8];
    uint16 checksumTotal, scanInterval;
    int    sendBuffSize, sendChars, recChars, i;

    sendBuffSize = 12 + NumChannels * 2;

    sendBuff[1] = (uint8)(0xF8);  // Command byte
    sendBuff[2] = 3 + NumChannels;  // Number of data words = NumChannels + 3
    sendBuff[3] = (uint8)(0x11);  // Extended command number
    sendBuff[6] = NumChannels;  // NumChannels
    sendBuff[7] = SamplesPerPacket;  // SamplesPerPacket
    sendBuff[8] = 0;  // Reserved
    sendBuff[9] = 1;  // ScanConfig:
                      // Bit 7: Reserved
                      // Bit 6: Reserved
                      // Bit 3: Internal stream clock frequency = b0: 4 MHz
                      // Bit 2: Divide Clock by 256 = b0
                      // Bits 0-1: Resolution = b01: 11.9-bit effective

    scanInterval = 4000;
    sendBuff[10] = (uint8)(scanInterval & (0x00FF));  // Scan interval (low
                                                      // byte)
    sendBuff[11] = (uint8)(scanInterval / 256);  // Scan interval (high byte)

    for (i = 0; i < NumChannels; i++)
    {
        sendBuff[12 + i * 2] = i;  // PChannel = i
        sendBuff[13 + i * 2] = 31;  // NChannel = 31: Single Ended
    }

    extendedChecksum(sendBuff, sendBuffSize);

    // Sending command to U3
    sendChars = LJUSB_Write(hDevice, sendBuff, sendBuffSize);
    if (sendChars < sendBuffSize)
    {
        if (sendChars == 0)
            printf("Error : write failed (StreamConfig).\n");
        else
            printf("Error : did not write all of the buffer (StreamConfig).\n");
        return -1;
    }

    for (i = 0; i < 8; i++) recBuff[i] = 0;

    // Reading response from U3
    recChars = LJUSB_Read(hDevice, recBuff, 8);
    if (recChars < 8)
    {
        if (recChars == 0)
            printf("Error : read failed (StreamConfig).\n");
        else
            printf(
                "Error : did not read all of the buffer, %d (StreamConfig).\n",
                recChars);

        for (i = 0; i < 8; i++) printf("%d ", recBuff[i]);

        return -1;
    }

    checksumTotal = extendedChecksum16(recBuff, 8);
    if ((uint8)((checksumTotal / 256) & 0xFF) != recBuff[5])
    {
        printf("Error : read buffer has bad checksum16(MSB) (StreamConfig).\n");
        return -1;
    }

    if ((uint8)(checksumTotal & 0xFF) != recBuff[4])
    {
        printf("Error : read buffer has bad checksum16(LBS) (StreamConfig).\n");
        return -1;
    }

    if (extendedChecksum8(recBuff) != recBuff[0])
    {
        printf("Error : read buffer has bad checksum8 (StreamConfig).\n");
        return -1;
    }

    if (recBuff[1] != (uint8)(0xF8) || recBuff[2] != (uint8)(0x01) ||
        recBuff[3] != (uint8)(0x11) || recBuff[7] != (uint8)(0x00))
    {
        printf("Error : read buffer has wrong command bytes (StreamConfig).\n");
        return -1;
    }

    if (recBuff[6] != 0)
    {
        printf(
            "Errorcode # %d from StreamConfig read.\n",
            (unsigned int)recBuff[6]);
        return -1;
    }

    return 0;
}

// Sends a StreamStart low-level command to start streaming.
int StreamStart(HANDLE hDevice)
{
    uint8 sendBuff[2], recBuff[4];
    int   sendChars, recChars;

    sendBuff[0] = (uint8)(0xA8);  // CheckSum8
    sendBuff[1] = (uint8)(0xA8);  // command byte

    // Sending command to U3
    sendChars = LJUSB_Write(hDevice, sendBuff, 2);
    if (sendChars < 2)
    {
        if (sendChars == 0)
            printf("Error : write failed.\n");
        else
            printf("Error : did not write all of the buffer.\n");
        return -1;
    }

    // Reading response from U3
    recChars = LJUSB_Read(hDevice, recBuff, 4);
    if (recChars < 4)
    {
        if (recChars == 0)
            printf("Error : read failed.\n");
        else
            printf("Error : did not read all of the buffer.\n");
        return -1;
    }

    if (normalChecksum8(recBuff, 4) != recBuff[0])
    {
        printf("Error : read buffer has bad checksum8 (StreamStart).\n");
        return -1;
    }

    if (recBuff[1] != (uint8)(0xA9) || recBuff[3] != (uint8)(0x00))
    {
        printf("Error : read buffer has wrong command bytes \n");
        return -1;
    }

    if (recBuff[2] != 0)
    {
        printf(
            "Errorcode # %d from StreamStart read.\n",
            (unsigned int)recBuff[2]);
        return -1;
    }

    return 0;
}

// Reads the StreamData low-level function response in a loop.  All voltages
// from the stream are stored in the voltages 2D array.
void LabjackNode::onReadAndPubTimer()
{
    uint16 voltageBytes, checksumTotal;
    double hardwareVersion;
    int    recBuffSize, recChars, autoRecoveryOn;
    int    currChannel, scanNumber;
    int    j, k, m;
    int    totalPackets;  // The total number of StreamData responses read

    // Number of packets to read before displaying streaming information
    constexpr int numReadsPerDisplay = 1;

    // Multiplier for the StreamData receive buffer size
    constexpr int readSizeMultiplier = 5;
    // The number of bytes in a StreamData response (differs with
    // SamplesPerPacket)
    constexpr int responseSize = 14 + SamplesPerPacket * 2;

    /* Each StreamData response contains (SamplesPerPacket / NumChannels) *
     * readSizeMultiplier samples for each channel. Total number of scans =
     * (SamplesPerPacket / NumChannels) * readSizeMultiplier *
     * numReadsPerDisplay * numDisplay
     */
    double voltages
        [(SamplesPerPacket / NumChannels) * readSizeMultiplier *
         numReadsPerDisplay][NumChannels];
    uint8 recBuff[responseSize * readSizeMultiplier];

    currChannel     = 0;
    scanNumber      = 0;
    totalPackets    = 0;
    recChars        = 0;
    autoRecoveryOn  = 0;
    recBuffSize     = 14 + SamplesPerPacket * 2;
    hardwareVersion = caliInfo_.hardwareVersion;

    for (j = 0; j < numReadsPerDisplay; j++)
    {
        /* For USB StreamData, use Endpoint 3 for reads.  You can read the
         * multiple StreamData responses of 64 bytes only if
         * SamplesPerPacket is 25 to help improve streaming performance.  In
         * this example this multiple is adjusted by the readSizeMultiplier
         * variable.
         */

        // Reading stream response from U3
        recChars =
            LJUSB_Stream(hDevice_, recBuff, responseSize * readSizeMultiplier);
        if (recChars < responseSize * readSizeMultiplier)
        {
            if (recChars == 0)
                RCLCPP_ERROR(
                    get_logger(), "Error : read failed (StreamData).\n");
            else
                RCLCPP_ERROR(
                    get_logger(),
                    "Error : did not read all of the buffer, expected %d "
                    "bytes but received %d(StreamData).\n",
                    responseSize * readSizeMultiplier, recChars);
            return;
        }

        // Checking for errors and getting data out of each StreamData
        // response
        for (m = 0; m < readSizeMultiplier; m++)
        {
            totalPackets++;

            checksumTotal =
                extendedChecksum16(recBuff + m * recBuffSize, recBuffSize);
            if ((uint8)((checksumTotal / 256) & 0xFF) !=
                recBuff[m * recBuffSize + 5])
            {
                RCLCPP_ERROR(
                    get_logger(),
                    "Error : read buffer has bad checksum16(MSB) "
                    "(StreamData).\n");
                return;
            }

            if ((uint8)(checksumTotal & 0xFF) != recBuff[m * recBuffSize + 4])
            {
                RCLCPP_ERROR(
                    get_logger(),
                    "Error : read buffer has bad checksum16(LBS) "
                    "(StreamData).\n");
                return;
            }

            checksumTotal = extendedChecksum8(recBuff + m * recBuffSize);
            if (checksumTotal != recBuff[m * recBuffSize])
            {
                RCLCPP_ERROR(
                    get_logger(),
                    "Error : read buffer has bad checksum8 "
                    "(StreamData).\n");
                return;
            }

            if (recBuff[m * recBuffSize + 1] != (uint8)(0xF9) ||
                recBuff[m * recBuffSize + 2] != 4 + SamplesPerPacket ||
                recBuff[m * recBuffSize + 3] != (uint8)(0xC0))
            {
                RCLCPP_ERROR(
                    get_logger(),
                    "Error : read buffer has wrong command bytes "
                    "(StreamData).\n");
                return;
            }

            if (recBuff[m * recBuffSize + 11] == 59)
            {
                if (!autoRecoveryOn)
                {
                    printf(
                        "\nU3 data buffer overflow detected in packet "
                        "%d.\nNow using auto-recovery and reading buffered "
                        "samples.\n",
                        totalPackets);
                    autoRecoveryOn = 1;
                }
            }
            else if (recBuff[m * recBuffSize + 11] == 60)
            {
                printf(
                    "Auto-recovery report in packet %d: %d scans were "
                    "dropped.\nAuto-recovery is now off.\n",
                    totalPackets,
                    recBuff[m * recBuffSize + 6] +
                        recBuff[m * recBuffSize + 7] * 256);
                autoRecoveryOn = 0;
            }
            else if (recBuff[m * recBuffSize + 11] != 0)
            {
                RCLCPP_ERROR(
                    get_logger(), "Errorcode # %d from StreamData read.\n",
                    (unsigned int)recBuff[11]);
                return;
            }

#if 0  // JLBC: Allow streaming re-start, etc.
            if (packetCounter != (int)recBuff[m * recBuffSize + 10])
            {
                RCLCPP_ERROR(
                    get_logger(),
                    "PacketCounter (%d) does not match with with current "
                    "packet count (%d)(StreamData).\n",
                    recBuff[m * recBuffSize + 10], packetCounter);
                return;
            }

            backLog = (int)recBuff[m * 48 + 12 + SamplesPerPacket * 2];
#endif

            for (k = 12; k < (12 + SamplesPerPacket * 2); k += 2)
            {
                voltageBytes = (uint16)recBuff[m * recBuffSize + k] +
                               (uint16)recBuff[m * recBuffSize + k + 1] * 256;

                if (hardwareVersion >= 1.30)
                    getAinVoltCalibrated_hw130(
                        &caliInfo_, currChannel, 31, voltageBytes,
                        &(voltages[scanNumber][currChannel]));
                else
                    getAinVoltCalibrated(
                        &caliInfo_, dac1Enabled_, 31, voltageBytes,
                        &(voltages[scanNumber][currChannel]));

                currChannel++;
                if (currChannel >= NumChannels)
                {
                    currChannel = 0;
                    scanNumber++;
                }
            }

#if 0
            if (packetCounter >= 255)
                packetCounter = 0;
            else
                packetCounter++;
#endif
        }
    }

    RCLCPP_DEBUG(get_logger(), "Number of scans: %d\n", scanNumber);
    RCLCPP_DEBUG(get_logger(), "Total packets read: %d\n", totalPackets);

#if 0
    for (k = 0; k < NumChannels; k++)
        printf("  AI%d: %.4f V\n", k, voltages[scanNumber - 1][k]);
#endif

    std_msgs::msg::Float32MultiArray msgAdc;
    msgAdc.data.resize(NumChannels);

    for (k = 0; k < NumChannels; k++)
        msgAdc.data[k] = voltages[scanNumber - 1][k];

    adcPub_->publish(msgAdc);
}

// Sends a StreamStop low-level command to stop streaming.
int StreamStop(HANDLE hDevice)
{
    uint8 sendBuff[2], recBuff[4];
    int   sendChars, recChars;

    sendBuff[0] = (uint8)(0xB0);  // CheckSum8
    sendBuff[1] = (uint8)(0xB0);  // Command byte

    // Sending command to U3
    sendChars = LJUSB_Write(hDevice, sendBuff, 2);
    if (sendChars < 2)
    {
        if (sendChars == 0)
            printf("Error : write failed (StreamStop).\n");
        else
            printf("Error : did not write all of the buffer (StreamStop).\n");
        return -1;
    }

    // Reading response from U3
    recChars = LJUSB_Read(hDevice, recBuff, 4);
    if (recChars < 4)
    {
        if (recChars == 0)
            printf("Error : read failed (StreamStop).\n");
        else
            printf("Error : did not read all of the buffer (StreamStop).\n");
        return -1;
    }

    if (normalChecksum8(recBuff, 4) != recBuff[0])
    {
        printf("Error : read buffer has bad checksum8 (StreamStop).\n");
        return -1;
    }

    if (recBuff[1] != (uint8)(0xB1) || recBuff[3] != (uint8)(0x00))
    {
        printf("Error : read buffer has wrong command bytes (StreamStop).\n");
        return -1;
    }

    if (recBuff[2] != 0)
    {
#if 0
        printf(
            "Errorcode # %d from StreamStop read.\n", (unsigned int)recBuff[2]);
#endif
        return -1;
    }

    return 0;
}
