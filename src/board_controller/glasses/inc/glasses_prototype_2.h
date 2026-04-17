#pragma once

#include <thread>

#include "board.h"
#include "serial.h"


class GLASSESPROTOTYPE2 : public Board
{

private:
    volatile bool keep_alive;
    bool initialized;
    bool is_streaming;
    std::thread streaming_thread;

    Serial *serial;

    int open_port ();
    void read_thread ();

public:
    GLASSESPROTOTYPE2 (struct BrainFlowInputParams params);
    ~GLASSESPROTOTYPE2 ();

    int prepare_session ();
    int start_stream (int buffer_size, const char *streamer_params);
    int stop_stream ();
    int release_session ();
    int config_board (std::string config, std::string &response);
    int config_board_with_bytes (const char *bytes, int len);
};