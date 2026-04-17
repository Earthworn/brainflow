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
    ((GLASSESPROTOTYPE2 *)(board))->read_thread ();
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
}

int GLASSESPROTOTYPE2::start_stream (int buffer_size, const char *streamer_params)
{
    // TODO;
}

int GLASSESPROTOTYPE2::stop_stream ()
{
    // TODO
}

int GLASSESPROTOTYPE2::release_session ()
{
    // TODO
}

void GLASSESPROTOTYPE2::read_thread ()
{
    /*
    Byte 1: Data value for EEG channel 1
    ,
    Byte 2: Data value for EEG channel 2
    ,
    Byte 3: Data value for EEG channel 3
    ,
    Byte 4: Data value for EEG channel 4
    ,
    \n
    */
    int num_rows = board_descr["default"]["num_rows"];
    double *package = new double[num_rows];
    // 4.5 Ref Voltage:
    double vref = 4.5;
    // Gain is hardcoded to 12
    double gain = 1;
    // 2^23 -1 because of sigend 24bit values
    double resolution_factor = (int)(pow (2, 23) - 1);
    // LSB = Vref/ (gain * 2^23 -1)
    // * 1.000.000 to convert to microvolt
    int micro = 1000000;
    double eeg_scale = (double)(vref / resolution_factor / gain * 1000000.);
    std::vector<int> eeg_channels = board_descr["default"]["eeg_channels"];

    while (keep_alive)
    {
        // Read Line
        std::string line;
        unsigned char c;
        while (true)
        {
            char current_char = serial->read_from_serial_port (&c, 1);
            if (current_char != 1)
                continue;
            if (c == '\n')
                break;
            line += c;
        }

        if (!line.empty () && line.back () == '\r')
            line.pop_back ();

        std::vector<int> values;

        std::string delimiter = ", ";
        size_t start = 0;
        size_t end = line.find (delimiter);

        while (end != std::string::npos)
        {
            values.push_back (std::stoi (line.substr (start, end - start)));
            start = end + delimiter.length ();
            end = line.find (delimiter, start);
        }
        values.push_back (std::stoi (line.substr (start)));

        if (values.size () != eeg_channels.size ())
        {
            continue;
        }
        // eeg
        for (unsigned int i = 0; i < eeg_channels.size (); i++)
        {
            package[eeg_channels[i]] = eeg_scale * values[i];
        }
        push_package (package);
    }
    delete[] package;
}

int GLASSESPROTOTYPE2::config_board (std::string config, std::string &response)
{
    return (int)BrainFlowExitCodes::UNSUPPORTED_BOARD_ERROR;
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
            if (strncmp (peripheral_identified, "Ganglion", 8) == 0)
            {
                found = true;
            }
            // for some reason device may send Simblee instead Ganglion name
            else if (strncmp (peripheral_identified, "Simblee", 7) == 0)
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