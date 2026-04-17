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

#include "glasses_prototype_1.h"
#include "timestamp.h"


GLASSESPROTOTYPE1::GLASSESPROTOTYPE1 (struct BrainFlowInputParams params)
    : Board ((int)BoardIds::GLASSES_PROTOTYPE_1, params)
{
    is_streaming = false;
    keep_alive = false;
    initialized = false;
}

GLASSESPROTOTYPE1::~GLASSESPROTOTYPE1 ()
{
    skip_logs = true;
    release_session ();
}
int GLASSESPROTOTYPE1::open_port ()
{
    if (serial->is_port_open ())
    {
        safe_logger (spdlog::level::err, "port {} already open", serial->get_port_name ());
        return (int)BrainFlowExitCodes::PORT_ALREADY_OPEN_ERROR;
    }

    safe_logger (spdlog::level::info, "opening port {}", serial->get_port_name ());
    int res = serial->open_serial_port ();
    if (res < 0)
    {
        safe_logger (spdlog::level::err,
            "Make sure you provided correct port name and have permissions to open it(run with "
            "sudo/admin). Also, close all other apps using this port.");
        return (int)BrainFlowExitCodes::UNABLE_TO_OPEN_PORT_ERROR;
    }
    safe_logger (spdlog::level::trace, "port {} is open", serial->get_port_name ());
    return (int)BrainFlowExitCodes::STATUS_OK;
}

int GLASSESPROTOTYPE1::prepare_session ()
{
    if (initialized)
    {
        safe_logger (spdlog::level::info, "Session is already prepared");
        return (int)BrainFlowExitCodes::STATUS_OK;
    }
    if (params.serial_port.empty ())
    {
        safe_logger (spdlog::level::err, "serial port is empty");
        return (int)BrainFlowExitCodes::INVALID_ARGUMENTS_ERROR;
    }
    safe_logger (spdlog::level::info, "Create Serial Port");

    serial = Serial::create (params.serial_port.c_str (), this);
    serial->set_custom_baudrate (9600);


    int port_open = open_port ();
    if (port_open != (int)BrainFlowExitCodes::STATUS_OK)
    {
        delete serial;
        serial = NULL;
        return port_open;
    }

    initialized = true;
    return (int)BrainFlowExitCodes::STATUS_OK;
}

int GLASSESPROTOTYPE1::start_stream (int buffer_size, const char *streamer_params)
{
    if (is_streaming)
    {
        safe_logger (spdlog::level::err, "Streaming thread already running");
        return (int)BrainFlowExitCodes::STREAM_ALREADY_RUN_ERROR;
    }


    int res = prepare_for_acquisition (buffer_size, streamer_params);
    if (res != (int)BrainFlowExitCodes::STATUS_OK)
    {
        return res;
    }

    keep_alive = true;
    streaming_thread = std::thread ([this] { this->read_thread (); });
    is_streaming = true;
    safe_logger (spdlog::level::info, "Started Session");

    return (int)BrainFlowExitCodes::STATUS_OK;
}

int GLASSESPROTOTYPE1::stop_stream ()
{
    if (is_streaming)
    {
        keep_alive = false;
        is_streaming = false;
        if (streaming_thread.joinable ())
        {
            streaming_thread.join ();
        }
        return (int)BrainFlowExitCodes::STATUS_OK;
    }
    else
    {
        return (int)BrainFlowExitCodes::STREAM_THREAD_IS_NOT_RUNNING;
    }
}

int GLASSESPROTOTYPE1::release_session ()
{
    if (initialized)
    {
        stop_stream ();
        free_packages ();
        initialized = false;
    }
    if (serial)
    {
        serial->close_serial_port ();
        delete serial;
        serial = NULL;
        safe_logger (spdlog::level::info, "Closed Com port");
    }
    return (int)BrainFlowExitCodes::STATUS_OK;
}

void GLASSESPROTOTYPE1::read_thread ()
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

int GLASSESPROTOTYPE1::config_board (std::string config, std::string &response)
{
    return (int)BrainFlowExitCodes::UNSUPPORTED_BOARD_ERROR;
}

int GLASSESPROTOTYPE1::config_board_with_bytes (const char *bytes, int len)
{
    return (int)BrainFlowExitCodes::UNSUPPORTED_BOARD_ERROR;
}
