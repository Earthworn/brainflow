#include <chrono>
#include <fstream>
#include <math.h>
#include <random>
#include <string.h>
#include <string>
#include <vector>

#ifdef _WIN32
#include <windows.h>
#else
#include <unistd.h>
#endif

#include "glasses_prototype_2.h"
#include "timestamp.h"


#define GLASSES_PROTOTYPE_2_SERIVCE_UUID "8f7e6d5c-4b3a-2918-a7b6-c5d4e3f20100"
#define GLASSES_PROTOTYPE_2_WRITE_UUID "8f7e6d5c-4b3a-2918-a7b6-c5d4e3f20101"
#define GLASSES_PROTOTYPE_2_READ_NOTIFY_UUID "8f7e6d5c-4b3a-2918-a7b6-c5d4e3f20102"

#define GLASSES_PROTOTYPE_2_IDENTIFIER "EEG Glasses Prototype 2"


static void glasses_adapter_1_on_scan_start (simpleble_adapter_t adapter, void *board)
{
    ((GLASSESPROTOTYPE2 *)(board))->adapter_1_on_scan_start (adapter);
}

static void glasses_adapter_1_on_scan_stop (simpleble_adapter_t adapter, void *board)
{
    ((GLASSESPROTOTYPE2 *)(board))->adapter_1_on_scan_stop (adapter);
}

static void glasses_adapter_1_on_scan_found (
    simpleble_adapter_t adapter, simpleble_peripheral_t peripheral, void *board)
{
    ((GLASSESPROTOTYPE2 *)(board))->adapter_1_on_scan_found (adapter, peripheral);
}

static void glasses_read_notify_callback (simpleble_peripheral_t handle, simpleble_uuid_t service,
    simpleble_uuid_t characteristic, const uint8_t *data, size_t size, void *board)
{
    ((GLASSESPROTOTYPE2 *)(board))
        ->read_thread (simpleble_uuid_t (service), simpleble_uuid_t (characteristic), data, size);
}

GLASSESPROTOTYPE2::GLASSESPROTOTYPE2 (struct BrainFlowInputParams params)
    : BLELibBoard ((int)BoardIds::GLASSES_PROTOTYPE_2, params)
{
    initialized = false;
    glasses_adapter = NULL;
    glasses_peripheral = NULL;
    is_streaming = false;
}

GLASSESPROTOTYPE2::~GLASSESPROTOTYPE2 ()
{
    skip_logs = true;
    release_session ();
}

int GLASSESPROTOTYPE2::prepare_session ()
{
    if (initialized)
    {
        safe_logger (spdlog::level::info, "Session is already prepared");
        return (int)BrainFlowExitCodes::STATUS_OK;
    }

    if (!init_dll_loader ())
    {
        safe_logger (spdlog::level::err, "Failed to init dll_loader");
        return (int)BrainFlowExitCodes::GENERAL_ERROR;
    }

    size_t adapter_count = simpleble_adapter_get_count ();
    if (adapter_count == 0)
    {
        safe_logger (spdlog::level::err, "No Bluetooth adapters found");
        return (int)BrainFlowExitCodes::UNABLE_TO_OPEN_PORT_ERROR;
    }

    glasses_adapter = simpleble_adapter_get_handle (0);
    if (glasses_adapter == NULL)
    {
        safe_logger (spdlog::level::err, "Failed to get Bluetooth adapter");
        return (int)BrainFlowExitCodes::UNABLE_TO_OPEN_PORT_ERROR;
    }

    simpleble_adapter_set_callback_on_scan_start (
        glasses_adapter, ::glasses_adapter_1_on_scan_start, (void *)this);

    simpleble_adapter_set_callback_on_scan_stop (
        glasses_adapter, ::glasses_adapter_1_on_scan_stop, (void *)this);

    simpleble_adapter_set_callback_on_scan_found (
        glasses_adapter, ::glasses_adapter_1_on_scan_found, (void *)this);

#ifdef _WIN32
    Sleep (1000);
#else
    usleep (1000000);
#endif

    if (!simpleble_adapter_is_bluetooth_enabled ())
    {
        safe_logger (spdlog::level::err, "Bluetooth is not enabled");
    }

    simpleble_adapter_scan_start (glasses_adapter);

    int res = (int)BrainFlowExitCodes::STATUS_OK;

    std::unique_lock<std::mutex> lk (m);
    auto sec = std::chrono::seconds (1);

    if (!cv.wait_for (lk, sec, [this] { return glasses_peripheral != NULL; }))
    {
        safe_logger (spdlog::level::err, "Failed to find glasses during scan");
        res = (int)BrainFlowExitCodes::UNABLE_TO_OPEN_PORT_ERROR;
    }
    else
    {
        safe_logger (spdlog::level::info, "Glasses found during scan");
    }


    simpleble_adapter_scan_stop (glasses_adapter);

    if (res == (int)BrainFlowExitCodes::STATUS_OK)
    {
        // for safety

        for (int i = 0; i < 3; i++)
        {
            if (simpleble_peripheral_connect (glasses_peripheral) == SIMPLEBLE_SUCCESS)
            {
                safe_logger (spdlog::level::info, "Connected to glasses");
                res = (int)BrainFlowExitCodes::STATUS_OK;
                break;
            }
            else
            {
                safe_logger (spdlog::level::warn, "Failed to connect to glasses: {}/3", i);
                res = (int)BrainFlowExitCodes::BOARD_NOT_READY_ERROR;
#ifdef _WIN32
                Sleep (1000);
#else
                sleep (1);
#endif
            }
        }
    }

    int num_chars_found = 0;

    if (res == (int)BrainFlowExitCodes::STATUS_OK)
    {
        size_t services_count = simpleble_peripheral_services_count (glasses_peripheral);
        for (size_t i = 0; i < services_count; i++)
        {
            simpleble_service_t service;
            if (simpleble_peripheral_services_get (glasses_peripheral, i, &service) !=
                SIMPLEBLE_SUCCESS)
            {
                safe_logger (
                    spdlog::level::err, "Failed to get service {} for glasses", service.uuid.value);
                res = (int)BrainFlowExitCodes::GENERAL_ERROR;
            }

            if (strcmp (service.uuid.value, GLASSES_PROTOTYPE_2_SERIVCE_UUID) == 0)
            {
                for (size_t j = 0; j < service.characteristic_count; j++)
                {
                    if (strcmp (service.characteristics[j].uuid.value,
                            GLASSES_PROTOTYPE_2_READ_NOTIFY_UUID) == 0)
                    {
                        if (simpleble_peripheral_notify (glasses_peripheral, service.uuid,
                                service.characteristics[j].uuid, ::glasses_read_notify_callback,
                                (void *)this) == SIMPLEBLE_SUCCESS)
                        {
                            read_notified_characteristics =
                                std::pair<simpleble_uuid_t, simpleble_uuid_t> (
                                    service.uuid, service.characteristics[j].uuid);
                            num_chars_found++;
                        }
                        else
                        {
                            safe_logger (spdlog::level::err, "Failed to notify for {} {}",
                                service.uuid.value, service.characteristics[j].uuid.value);
                            res = (int)BrainFlowExitCodes::GENERAL_ERROR;
                        }
                    }
                    if (strcmp (service.characteristics[j].uuid.value,
                            GLASSES_PROTOTYPE_2_WRITE_UUID) == 0)
                    {
                        write_characteristics = std::pair<simpleble_uuid_t, simpleble_uuid_t> (
                            service.uuid, service.characteristics[j].uuid);
                        num_chars_found++;
                    }
                }
            }
        }
    }

    if (res == (int)BrainFlowExitCodes::STATUS_OK)
    {
        initialized = true;
    }
}

int GLASSESPROTOTYPE2::start_stream (int buffer_size, const char *streamer_params)
{
    if (!initialized)
    {
        return (int)BrainFlowExitCodes::BOARD_NOT_CREATED_ERROR;
    }
    if (is_streaming)
    {
        return (int)BrainFlowExitCodes::STREAM_ALREADY_RUN_ERROR;
    }
    int res = prepare_for_acquisition (buffer_size, streamer_params);
    if (res == (int)BrainFlowExitCodes::STATUS_OK)
    {
        res = send_command ("b");
    }
    if (res == (int)BrainFlowExitCodes::STATUS_OK)
    {
        is_streaming = true;
    }

    return res;
}

int GLASSESPROTOTYPE2::stop_stream ()
{
    if (glasses_peripheral == NULL)
    {
        return (int)BrainFlowExitCodes::BOARD_NOT_CREATED_ERROR;
    }
    int res = (int)BrainFlowExitCodes::STATUS_OK;
    if (is_streaming)
    {
        res = send_command ("s");
    }
    else
    {
        res = (int)BrainFlowExitCodes::STREAM_THREAD_IS_NOT_RUNNING;
    }
    is_streaming = false;
    return res;
}

int GLASSESPROTOTYPE2::release_session ()
{
    if (initialized)
    {
        // repeat it multiple times, failure here may lead to a crash
        for (int i = 0; i < 2; i++)
        {
            stop_stream ();
            // need to wait for notifications to stop triggered before unsubscribing, otherwise
            // macos fails inside simpleble with timeout
#ifdef _WIN32
            Sleep (2000);
#else
            sleep (2);
#endif
            if (simpleble_peripheral_unsubscribe (glasses_peripheral,
                    read_notified_characteristics.first,
                    read_notified_characteristics.second) != SIMPLEBLE_SUCCESS)
            {
                safe_logger (spdlog::level::err, "failed to unsubscribe for {} {}",
                    read_notified_characteristics.first.value,
                    read_notified_characteristics.second.value);
            }
            else
            {
                break;
            }
        }
        free_packages ();
        initialized = false;
    }
    if (glasses_peripheral != NULL)
    {
        bool is_connected = false;
        if (simpleble_peripheral_is_connected (glasses_peripheral, &is_connected) ==
            SIMPLEBLE_SUCCESS)
        {
            if (is_connected)
            {
                simpleble_peripheral_disconnect (glasses_peripheral);
            }
        }
        simpleble_peripheral_release_handle (glasses_peripheral);
        glasses_peripheral = NULL;
    }
    if (glasses_adapter != NULL)
    {
        simpleble_adapter_release_handle (glasses_adapter);
        glasses_adapter = NULL;
    }

    return (int)BrainFlowExitCodes::STATUS_OK;
}

int GLASSESPROTOTYPE2::send_command (std::string config)
{
    if (!initialized)
    {
        return (int)BrainFlowExitCodes::BOARD_NOT_CREATED_ERROR;
    }

    if (config.empty ())
    {
        return (int)BrainFlowExitCodes::INVALID_ARGUMENTS_ERROR;
    }

    uint8_t *command = new uint8_t[config.size ()];
    memcpy (command, config.c_str (), config.size ());
    if (simpleble_peripheral_write_command (glasses_peripheral, write_characteristics.first,
            write_characteristics.second, command, config.size ()) != SIMPLEBLE_SUCCESS)
    {
        safe_logger (spdlog::level::err, "failed to send command {} to device", config.c_str ());
        delete[] command;
        return (int)BrainFlowExitCodes::BOARD_WRITE_ERROR;
    }
    delete[] command;
    return (int)BrainFlowExitCodes::STATUS_OK;
}

void GLASSESPROTOTYPE2::read_thread (
    simpleble_uuid_t service, simpleble_uuid_t characteristic, const uint8_t *data, size_t size)
{
    /*
    One Notification should contains:
    5 eeg packages
    3 accel values each 16 bit signed int
    3 gyro values each 16 bit signed int
    Every eeg package contains 9 bytes:
    First 24 bits:  (1100 + LOFF_STATP + LOFF_STATN + bits[4:7] of the GPIO register).
    ,
    8 times 24 bits Data values for EEG channel 1-8
    */
    int num_packages = 5;
    int num_rows = board_descr["default"]["num_rows"];
    double *package = new double[num_rows];

    // 4.5 Ref Voltage:
    double vref = 4.5;
    // 2^23 -1 because of sigend 24bit values
    double resolution_factor = (int)(pow (2, 23) - 1);
    // LSB = Vref/ (gain * 2^23 -1)
    // * 1.000.000 to convert to microvolt
    int micro = 1000000;
    double eeg_scale = (double)(vref / resolution_factor / gain * 1000000.);
    std::vector<int> eeg_channels = board_descr["default"]["eeg_channels"];
    std::vector<int> accel_channels = board_descr["default"]["accel_channels"];
    std::vector<int> gyro_channels = board_descr["default"]["gyro_channels"];


    std::vector<uint8_t> one_package;

    // Get accel and gyro values one time per notification, because they are the same for all eeg
    // packages in one notification

    int16_t accel_gyro_values[6];
    for (size_t i = 0; i < 6; i++)
    {
        accel_gyro_values[i] = (int16_t)((data[3 + eeg_channels.size () * 3 + i * 2] << 8) |
            data[3 + eeg_channels.size () * 3 + 1 + i * 2]);
    }


    for (size_t i = 0; i < num_packages; i++)
    {
        one_package = std::vector<uint8_t> (data + i * num_packages, data + (i + 1) * num_packages);

        int32_t status_value = (one_package[0] << 16) | (one_package[1] << 8) | one_package[2];

        // extract EEG values and convert to microvolts
        for (size_t j = 0; j < eeg_channels.size (); j++)
        {
            int32_t eeg_value = (one_package[3 + j * 3] << 16) | (one_package[4 + j * 3] << 8) |
                one_package[5 + j * 3];
            // convert to microvolts
            package[eeg_channels[j]] = (double)eeg_value * eeg_scale;
        }

        // Get accel and gyro values
        for (size_t j = 0; j < accel_channels.size (); j++)
        {
            package[accel_channels[j]] = (double)accel_gyro_values[j];
        }
        for (size_t j = 0; j < gyro_channels.size (); j++)
        {
            package[gyro_channels[j]] = (double)accel_gyro_values[3 + j];
        }

        push_package (package);
        // clear package for next iteration
        for (int k = 0; k < num_rows; k++)
        {
            package[k] = 0.0;
        }
    }

    delete[] package;
}

int GLASSESPROTOTYPE2::config_board (std::string config, std::string &response)
{
    if (!(update_gain_from_config (config) == 0))
    {
        return (int)BrainFlowExitCodes::INVALID_ARGUMENTS_ERROR;
    }
    return send_command (config);
}

int GLASSESPROTOTYPE2::update_gain_from_config (std::string config)
{
    // command can be x (CHANNEL, POWER_DOWN, GAIN_SET, INPUT_TYPE_SET, BIAS_SET, SRB2_SET,
    // SRB1_SET) X 		○ Gain_Set:
    //          0 = Gain 1
    //          1 = Gain 2
    //          2 = Gain 4
    //          3 = Gain 6
    //          4 = Gain 8
    //          5 = Gain 12
    //          6 = Gain 24(default)

    if (config.size () != 0 && config[0] != 'x' || config[8] != 'X')
    {
        // No gain update
        return 0;
    }

    int config_gain = config[4] - '0';
    if (config_gain < 0 || config_gain > 6)
    {
        safe_logger (spdlog::level::err, "Invalid gain value in config: {}", config);
        return (int)BrainFlowExitCodes::INVALID_ARGUMENTS_ERROR;
    }
    switch (config_gain)
    {
        case 0:
            gain = 1;
            break;
        case 1:
            gain = 2;
            break;
        case 2:
            gain = 4;
            break;
        case 3:
            gain = 6;
            break;
        case 4:
            gain = 8;
            break;
        case 5:
            gain = 12;
            break;
        case 6:
            gain = 24;
            break;
        default:
            safe_logger (spdlog::level::err, "Invalid gain value in config: {}", config);
            return (int)BrainFlowExitCodes::INVALID_ARGUMENTS_ERROR;
    }
}

int GLASSESPROTOTYPE2::config_board_with_bytes (const char *bytes, int len)
{
    return (int)BrainFlowExitCodes::UNSUPPORTED_BOARD_ERROR;
}

void GLASSESPROTOTYPE2::adapter_1_on_scan_start (simpleble_adapter_t adapter)
{
    safe_logger (spdlog::level::info, "Scan started");
}

void GLASSESPROTOTYPE2::adapter_1_on_scan_stop (simpleble_adapter_t adapter)
{
    safe_logger (spdlog::level::info, "Scan stopped");
}

void GLASSESPROTOTYPE2::adapter_1_on_scan_found (
    simpleble_adapter_t adapter, simpleble_peripheral_t peripheral)
{
    char *peripheral_identified = simpleble_peripheral_identifier (peripheral);
    char *peripheral_address = simpleble_peripheral_address (peripheral);
    bool found = false;

    if (!params.mac_address.empty ())
    {
        if (strcmp (peripheral_address, params.mac_address.c_str ()) == 0)
        {
            found = true;
        }
    }
    else
    {
        if (!params.serial_number.empty ())
        {
            if (strcmp (peripheral_identified, params.serial_number.c_str ()) == 0)
            {
                found = true;
            }
        }
        else
        {
            // Identify by name and mac address
            if (strcmp (peripheral_identified, GLASSES_PROTOTYPE_2_IDENTIFIER) == 0)
            {
                found = true;
            }
        }
    }

    safe_logger (spdlog::level::trace, "address {}", peripheral_address);
    simpleble_free (peripheral_address);
    safe_logger (spdlog::level::trace, "identifier {}", peripheral_identified);
    simpleble_free (peripheral_identified);

    if (found)
    {
        {
            std::lock_guard<std::mutex> lk (m);
            glasses_peripheral = peripheral;
        }
        cv.notify_one ();
    }
    else
    {
        simpleble_peripheral_release_handle (peripheral);
    }
}