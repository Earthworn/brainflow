#pragma once

#include <condition_variable>
#include <mutex>
#include <thread>

#include "ble_lib_board.h"
#include "board.h"


class GLASSESPROTOTYPE2 : public BLELibBoard
{

private:
    volatile bool keep_alive;
    bool initialized;
    bool is_streaming;
    std::thread streaming_thread;
    volatile simpleble_peripheral_t glasses_peripheral;
    volatile simpleble_adapter_t glasses_adapter;
    std::pair<simpleble_uuid_t, simpleble_uuid_t> read_notified_characteristics;
    std::pair<simpleble_uuid_t, simpleble_uuid_t> write_characteristics;
    std::mutex m;
    std::condition_variable cv;


public:
    GLASSESPROTOTYPE2 (struct BrainFlowInputParams params);
    ~GLASSESPROTOTYPE2 ();

    int prepare_session ();
    int start_stream (int buffer_size, const char *streamer_params);
    int stop_stream ();
    int release_session ();
    int config_board (std::string config, std::string &response);
    int config_board_with_bytes (const char *bytes, int len);
    void read_thread ();
    int send_command (std::string config);

    void adapter_1_on_scan_start (simpleble_adapter_t adapter);
    void adapter_1_on_scan_stop (simpleble_adapter_t adapter);
    void adapter_1_on_scan_found (simpleble_adapter_t adapter, simpleble_peripheral_t peripheral);
};