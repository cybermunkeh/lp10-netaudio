/*
 * lp10-netaudio - direct TCP PCM to ALSA hw: playback
 *
 * This program deliberately has no resampler, mixer, DSP, volume control, or
 * ALSA plug layer.  A connection either maps exactly to the requested ALSA
 * hardware parameters or it is rejected.
 */

#define _POSIX_C_SOURCE 200112L

#include <alloca.h>
#include <alsa/asoundlib.h>
#include <arpa/inet.h>
#include <errno.h>
#include <netdb.h>
#include <signal.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

#define DEFAULT_CONFIG_PATH "/opt/lp10-netaudio/config.json"
#define PROTOCOL_HEADER_BYTES 32U
#define PROTOCOL_VERSION 1U
#define MAX_CLIENT_CHANNELS 8U
#define MAX_CONFIG_FRAMES 65536U
#define MAX_IO_BYTES (4U * 1024U * 1024U)

static const unsigned char protocol_magic[8] = {
    'L', 'P', '1', '0', 'N', 'A', 'U', '1'
};

enum wire_format {
    WIRE_S16_LE = 1,
    WIRE_S24_LE = 2,   /* 24 valid bits in a 32-bit little-endian container */
    WIRE_S24_3LE = 3, /* packed 24-bit little-endian samples */
    WIRE_S32_LE = 4
};

typedef struct {
    const char *name;
    unsigned int wire_id;
    unsigned int logical_bits;
    unsigned int physical_bytes;
    snd_pcm_format_t alsa_format;
} format_spec_t;

static const format_spec_t formats[] = {
    {"S16_LE", WIRE_S16_LE, 16, 2, SND_PCM_FORMAT_S16_LE},
    {"S24_LE", WIRE_S24_LE, 24, 4, SND_PCM_FORMAT_S24_LE},
    {"S24_3LE", WIRE_S24_3LE, 24, 3, SND_PCM_FORMAT_S24_3LE},
    {"S32_LE", WIRE_S32_LE, 32, 4, SND_PCM_FORMAT_S32_LE},
};

typedef struct {
    char listen_host[64];
    unsigned int listen_port;
    char alsa_device[128];
    unsigned int buffer_frames;
    unsigned int period_frames;
    char log_file[256];
} config_t;

typedef struct {
    unsigned int rate;
    unsigned int channels;
    const format_spec_t *format;
    unsigned int frame_bytes;
} stream_spec_t;

static volatile sig_atomic_t stop_requested = 0;
static FILE *log_fp = NULL;

static void log_message(const char *level, const char *fmt, ...) {
    char timestamp[32] = "unknown-time";
    time_t now = time(NULL);
    struct tm local_tm;
    va_list args;

    if (localtime_r(&now, &local_tm) != NULL) {
        (void)strftime(timestamp, sizeof(timestamp), "%Y-%m-%dT%H:%M:%S%z", &local_tm);
    }

    if (log_fp == NULL) {
        log_fp = stderr;
    }

    fprintf(log_fp, "%s [%s] ", timestamp, level);
    va_start(args, fmt);
    vfprintf(log_fp, fmt, args);
    va_end(args);
    fputc('\n', log_fp);
    fflush(log_fp);
}

static void on_signal(int signal_number) {
    (void)signal_number;
    stop_requested = 1;
}

/* Do not use signal(): Linux may install SA_RESTART, which would leave a
 * service stop waiting on an idle accept() or recv() call. */
static int install_signal_handlers(void) {
    struct sigaction action;
    memset(&action, 0, sizeof(action));
    action.sa_handler = on_signal;
    if (sigemptyset(&action.sa_mask) != 0 ||
        sigaction(SIGINT, &action, NULL) != 0 ||
        sigaction(SIGTERM, &action, NULL) != 0) {
        return -1;
    }
    return signal(SIGPIPE, SIG_IGN) == SIG_ERR ? -1 : 0;
}

static void config_defaults(config_t *config) {
    (void)snprintf(config->listen_host, sizeof(config->listen_host), "%s", "0.0.0.0");
    config->listen_port = 9100;
    (void)snprintf(config->alsa_device, sizeof(config->alsa_device), "%s", "hw:0,0");
    config->buffer_frames = 4096;
    config->period_frames = 1024;
    (void)snprintf(config->log_file, sizeof(config->log_file), "%s",
                   "/opt/lp10-netaudio/lp10-netaudio.log");
}

static const char *skip_whitespace(const char *cursor) {
    while (*cursor == ' ' || *cursor == '\t' || *cursor == '\r' || *cursor == '\n') {
        ++cursor;
    }
    return cursor;
}

/* Minimal JSON string parser. Config values may not contain control characters. */
static int parse_json_string(const char **cursor_ptr, char *output, size_t output_size) {
    const char *cursor = skip_whitespace(*cursor_ptr);
    size_t length = 0;

    if (*cursor != '"' || output_size == 0) {
        return -1;
    }
    ++cursor;

    while (*cursor != '\0' && *cursor != '"') {
        unsigned char character = (unsigned char)*cursor++;
        if (character < 0x20U || length + 1 >= output_size) {
            return -1;
        }
        if (character == '\\') {
            character = (unsigned char)*cursor++;
            switch (character) {
            case '"': case '\\': case '/': break;
            case 'b': character = '\b'; break;
            case 'f': character = '\f'; break;
            case 'n': character = '\n'; break;
            case 'r': character = '\r'; break;
            case 't': character = '\t'; break;
            default: return -1; /* Unicode escapes are intentionally not needed here. */
            }
        }
        output[length++] = (char)character;
    }

    if (*cursor != '"') {
        return -1;
    }
    output[length] = '\0';
    *cursor_ptr = cursor + 1;
    return 0;
}

static int parse_json_uint(const char **cursor_ptr, unsigned int *value) {
    const char *cursor = skip_whitespace(*cursor_ptr);
    unsigned long parsed = 0;
    bool saw_digit = false;

    while (*cursor >= '0' && *cursor <= '9') {
        saw_digit = true;
        if (parsed > (unsigned long)(UINT32_MAX / 10U)) {
            return -1;
        }
        parsed = parsed * 10U + (unsigned long)(*cursor - '0');
        if (parsed > UINT32_MAX) {
            return -1;
        }
        ++cursor;
    }
    if (!saw_digit) {
        return -1;
    }
    *value = (unsigned int)parsed;
    *cursor_ptr = cursor;
    return 0;
}

static int read_entire_file(const char *path, char **content_out) {
    FILE *file = fopen(path, "rb");
    long length;
    char *content;

    if (file == NULL) {
        return -1;
    }
    if (fseek(file, 0, SEEK_END) != 0 || (length = ftell(file)) < 0 ||
        fseek(file, 0, SEEK_SET) != 0 || length > 1024 * 1024) {
        (void)fclose(file);
        errno = EINVAL;
        return -1;
    }
    content = (char *)malloc((size_t)length + 1U);
    if (content == NULL) {
        (void)fclose(file);
        return -1;
    }
    if (fread(content, 1, (size_t)length, file) != (size_t)length) {
        free(content);
        (void)fclose(file);
        return -1;
    }
    content[length] = '\0';
    (void)fclose(file);
    *content_out = content;
    return 0;
}

static int parse_config(const char *path, config_t *config) {
    char *document = NULL;
    const char *cursor;
    unsigned int seen = 0;
    enum {
        SEEN_HOST = 1U << 0,
        SEEN_PORT = 1U << 1,
        SEEN_DEVICE = 1U << 2,
        SEEN_BUFFER = 1U << 3,
        SEEN_PERIOD = 1U << 4,
        SEEN_LOG = 1U << 5,
        SEEN_ALL = SEEN_HOST | SEEN_PORT | SEEN_DEVICE | SEEN_BUFFER | SEEN_PERIOD | SEEN_LOG
    };

    config_defaults(config);
    if (read_entire_file(path, &document) != 0) {
        return -1;
    }
    cursor = skip_whitespace(document);
    if (*cursor++ != '{') {
        goto invalid;
    }

    for (;;) {
        char key[64];
        unsigned int bit = 0;
        cursor = skip_whitespace(cursor);
        if (*cursor == '}') {
            ++cursor;
            break;
        }
        if (parse_json_string(&cursor, key, sizeof(key)) != 0) {
            goto invalid;
        }
        cursor = skip_whitespace(cursor);
        if (*cursor++ != ':') {
            goto invalid;
        }

        if (strcmp(key, "listen_host") == 0) {
            bit = SEEN_HOST;
            if (parse_json_string(&cursor, config->listen_host, sizeof(config->listen_host)) != 0) goto invalid;
        } else if (strcmp(key, "listen_port") == 0) {
            bit = SEEN_PORT;
            if (parse_json_uint(&cursor, &config->listen_port) != 0) goto invalid;
        } else if (strcmp(key, "alsa_device") == 0) {
            bit = SEEN_DEVICE;
            if (parse_json_string(&cursor, config->alsa_device, sizeof(config->alsa_device)) != 0) goto invalid;
        } else if (strcmp(key, "buffer_frames") == 0) {
            bit = SEEN_BUFFER;
            if (parse_json_uint(&cursor, &config->buffer_frames) != 0) goto invalid;
        } else if (strcmp(key, "period_frames") == 0) {
            bit = SEEN_PERIOD;
            if (parse_json_uint(&cursor, &config->period_frames) != 0) goto invalid;
        } else if (strcmp(key, "log_file") == 0) {
            bit = SEEN_LOG;
            if (parse_json_string(&cursor, config->log_file, sizeof(config->log_file)) != 0) goto invalid;
        } else {
            goto invalid; /* A typo must not silently select an unsafe default. */
        }
        if ((seen & bit) != 0U) {
            goto invalid;
        }
        seen |= bit;

        cursor = skip_whitespace(cursor);
        if (*cursor == ',') {
            ++cursor;
            continue;
        }
        if (*cursor == '}') {
            ++cursor;
            break;
        }
        goto invalid;
    }

    cursor = skip_whitespace(cursor);
    if (*cursor != '\0' || seen != SEEN_ALL || config->listen_port == 0 ||
        config->listen_port > 65535U ||
        config->buffer_frames == 0 || config->period_frames == 0 ||
        config->buffer_frames > MAX_CONFIG_FRAMES || config->period_frames > MAX_CONFIG_FRAMES ||
        config->period_frames > config->buffer_frames) {
        goto invalid;
    }
    free(document);
    return 0;

invalid:
    free(document);
    errno = EINVAL;
    return -1;
}

static bool direct_hw_device(const char *device) {
    return strncmp(device, "hw:", 3) == 0 && device[3] != '\0';
}

static bool protocol_rate_supported(unsigned int rate) {
    switch (rate) {
    case 44100: case 48000: case 88200: case 96000: case 176400: case 192000:
        return true;
    default:
        return false;
    }
}

static uint16_t load_be16(const unsigned char *data) {
    return (uint16_t)(((uint16_t)data[0] << 8) | (uint16_t)data[1]);
}

static uint32_t load_be32(const unsigned char *data) {
    return ((uint32_t)data[0] << 24) | ((uint32_t)data[1] << 16) |
           ((uint32_t)data[2] << 8) | (uint32_t)data[3];
}

static const format_spec_t *find_format(unsigned int wire_id) {
    size_t index;
    for (index = 0; index < sizeof(formats) / sizeof(formats[0]); ++index) {
        if (formats[index].wire_id == wire_id) {
            return &formats[index];
        }
    }
    return NULL;
}

/* Returns 1 on complete data, 0 on peer EOF before any byte, and -1 on error. */
static int recv_exact(int socket_fd, unsigned char *buffer, size_t bytes) {
    size_t received = 0;
    while (received < bytes) {
        ssize_t result = recv(socket_fd, buffer + received, bytes - received, 0);
        if (result == 0) {
            return received == 0 ? 0 : -1;
        }
        if (result < 0) {
            if (errno == EINTR && !stop_requested) {
                continue;
            }
            return -1;
        }
        received += (size_t)result;
    }
    return 1;
}

static int parse_stream_header(const unsigned char header[PROTOCOL_HEADER_BYTES], stream_spec_t *spec) {
    const format_spec_t *format;
    unsigned int expected_frame_bytes;

    if (memcmp(header, protocol_magic, sizeof(protocol_magic)) != 0 ||
        load_be16(header + 8) != PROTOCOL_VERSION ||
        load_be16(header + 10) != PROTOCOL_HEADER_BYTES ||
        load_be16(header + 22) != 1U || /* endian: 1 = little endian */
        load_be32(header + 28) != 0U) {  /* flags/reserved */
        return -1;
    }
    spec->rate = load_be32(header + 12);
    spec->channels = load_be16(header + 16);
    format = find_format(load_be16(header + 18));
    if (format == NULL || load_be16(header + 20) != format->logical_bits ||
        !protocol_rate_supported(spec->rate) || spec->channels == 0 ||
        spec->channels > MAX_CLIENT_CHANNELS) {
        return -1;
    }
    expected_frame_bytes = spec->channels * format->physical_bytes;
    if (load_be32(header + 24) != expected_frame_bytes) {
        return -1;
    }
    spec->format = format;
    spec->frame_bytes = expected_frame_bytes;
    return 0;
}

static int set_receive_timeout(int socket_fd, unsigned int seconds) {
    struct timeval timeout;
    timeout.tv_sec = (time_t)seconds;
    timeout.tv_usec = 0;
    return setsockopt(socket_fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
}

static int open_exact_alsa(const config_t *config, const stream_spec_t *stream,
                           snd_pcm_t **pcm_out) {
    snd_pcm_t *pcm = NULL;
    snd_pcm_hw_params_t *params;
    int error;
    int direction = 0;
    unsigned int actual_rate = 0;
    unsigned int actual_channels = 0;
    snd_pcm_format_t actual_format;
    snd_pcm_uframes_t period_frames = config->period_frames;
    snd_pcm_uframes_t buffer_frames = config->buffer_frames;

    if (!direct_hw_device(config->alsa_device)) {
        log_message("ERROR", "Refusing ALSA device '%s': only a direct hw:X,Y device is allowed",
                    config->alsa_device);
        return -1;
    }
    error = snd_pcm_open(&pcm, config->alsa_device, SND_PCM_STREAM_PLAYBACK, 0);
    if (error < 0) {
        log_message("ERROR", "Cannot open ALSA device %s: %s", config->alsa_device,
                    snd_strerror(error));
        return -1;
    }
    snd_pcm_hw_params_alloca(&params);
    if ((error = snd_pcm_hw_params_any(pcm, params)) < 0 ||
        (error = snd_pcm_hw_params_set_access(pcm, params, SND_PCM_ACCESS_RW_INTERLEAVED)) < 0 ||
        (error = snd_pcm_hw_params_set_rate_resample(pcm, params, 0)) < 0 ||
        (error = snd_pcm_hw_params_set_format(pcm, params, stream->format->alsa_format)) < 0 ||
        (error = snd_pcm_hw_params_set_rate(pcm, params, stream->rate, 0)) < 0 ||
        (error = snd_pcm_hw_params_set_channels(pcm, params, stream->channels)) < 0 ||
        (error = snd_pcm_hw_params_set_buffer_size_near(pcm, params, &buffer_frames)) < 0 ||
        (error = snd_pcm_hw_params_set_period_size_near(pcm, params, &period_frames, &direction)) < 0 ||
        (error = snd_pcm_hw_params(pcm, params)) < 0) {
        log_message("ERROR", "ALSA refuses exact stream %u Hz, %u channels, %s on %s: %s; no resampling was performed",
                    stream->rate, stream->channels, stream->format->name, config->alsa_device,
                    snd_strerror(error));
        snd_pcm_close(pcm);
        return -1;
    }

    (void)snd_pcm_hw_params_get_rate(params, &actual_rate, &direction);
    (void)snd_pcm_hw_params_get_channels(params, &actual_channels);
    (void)snd_pcm_hw_params_get_format(params, &actual_format);
    (void)snd_pcm_hw_params_get_period_size(params, &period_frames, &direction);
    (void)snd_pcm_hw_params_get_buffer_size(params, &buffer_frames);
    if (actual_rate != stream->rate || actual_channels != stream->channels ||
        actual_format != stream->format->alsa_format) {
        log_message("ERROR", "ALSA selected %u Hz, %u channels, %s instead of requested %u Hz, %u channels, %s; rejecting without conversion",
                    actual_rate, actual_channels, snd_pcm_format_name(actual_format), stream->rate,
                    stream->channels, stream->format->name);
        snd_pcm_close(pcm);
        return -1;
    }

    log_message("INFO", "ALSA direct hw stream configured: device=%s rate=%u channels=%u format=%s period_frames=%lu buffer_frames=%lu",
                config->alsa_device, actual_rate, actual_channels, stream->format->name,
                (unsigned long)period_frames, (unsigned long)buffer_frames);
    *pcm_out = pcm;
    return 0;
}

static int recover_pcm(snd_pcm_t *pcm, int error) {
    int result;
    if (error == -EPIPE) {
        log_message("WARN", "ALSA playback underrun; preparing the same direct hardware stream again");
        result = snd_pcm_prepare(pcm);
        return result;
    }
    if (error == -ESTRPIPE) {
        do {
            result = snd_pcm_resume(pcm);
            if (result == -EAGAIN) {
                sleep(1);
            }
        } while (result == -EAGAIN && !stop_requested);
        if (result < 0) {
            log_message("WARN", "ALSA resume failed; preparing the same direct hardware stream again");
            result = snd_pcm_prepare(pcm);
        }
        return result;
    }
    return error;
}

static int write_frames(snd_pcm_t *pcm, const unsigned char *data,
                        size_t frame_count, unsigned int frame_bytes) {
    size_t written = 0;
    while (written < frame_count && !stop_requested) {
        snd_pcm_sframes_t result = snd_pcm_writei(pcm,
            data + written * frame_bytes, (snd_pcm_uframes_t)(frame_count - written));
        if (result > 0) {
            written += (size_t)result;
            continue;
        }
        if (result == 0) {
            (void)snd_pcm_wait(pcm, 1000);
            continue;
        }
        result = recover_pcm(pcm, (int)result);
        if (result < 0) {
            log_message("ERROR", "ALSA write failed: %s", snd_strerror((int)result));
            return -1;
        }
    }
    return stop_requested ? -1 : 0;
}

static int handle_client(int client_fd, const config_t *config) {
    unsigned char header[PROTOCOL_HEADER_BYTES];
    stream_spec_t stream;
    snd_pcm_t *pcm = NULL;
    unsigned char *buffer = NULL;
    size_t capacity;
    size_t pending = 0;
    int header_result;
    int status = -1;

    if (set_receive_timeout(client_fd, 10) != 0) {
        log_message("WARN", "Could not set header receive timeout: %s", strerror(errno));
    }
    header_result = recv_exact(client_fd, header, sizeof(header));
    if (header_result <= 0) {
        if (header_result < 0 && !stop_requested) {
            log_message("WARN", "Incomplete or timed-out stream header: %s", strerror(errno));
        }
        return -1;
    }
    (void)set_receive_timeout(client_fd, 0);
    if (parse_stream_header(header, &stream) != 0) {
        log_message("ERROR", "Rejected invalid stream header (allowed: 44100/48000/88200/96000/176400/192000 Hz, LE PCM)");
        return -1;
    }
    log_message("INFO", "Accepted stream request: rate=%u channels=%u format=%s frame_bytes=%u",
                stream.rate, stream.channels, stream.format->name, stream.frame_bytes);

    if (open_exact_alsa(config, &stream, &pcm) != 0) {
        return -1;
    }
    capacity = (size_t)config->period_frames * stream.frame_bytes;
    if (capacity < stream.frame_bytes || capacity > MAX_IO_BYTES) {
        log_message("ERROR", "Configured period creates an unsafe I/O buffer size");
        goto cleanup;
    }
    buffer = (unsigned char *)malloc(capacity);
    if (buffer == NULL) {
        log_message("ERROR", "Cannot allocate %lu-byte PCM I/O buffer", (unsigned long)capacity);
        goto cleanup;
    }

    while (!stop_requested) {
        ssize_t received = recv(client_fd, buffer + pending, capacity - pending, 0);
        size_t total;
        size_t complete_bytes;
        if (received == 0) {
            if (pending != 0) {
                log_message("ERROR", "Sender ended with %lu incomplete PCM bytes; dropping only that partial frame",
                            (unsigned long)pending);
                goto cleanup;
            }
            if (snd_pcm_drain(pcm) < 0) {
                log_message("WARN", "ALSA drain failed at end of stream");
            }
            log_message("INFO", "PCM stream ended normally");
            status = 0;
            goto cleanup;
        }
        if (received < 0) {
            if (errno == EINTR && !stop_requested) {
                continue;
            }
            if (!stop_requested) {
                log_message("WARN", "Network read failed: %s", strerror(errno));
            }
            goto cleanup;
        }
        total = pending + (size_t)received;
        complete_bytes = (total / stream.frame_bytes) * stream.frame_bytes;
        if (complete_bytes != 0 &&
            write_frames(pcm, buffer, complete_bytes / stream.frame_bytes, stream.frame_bytes) != 0) {
            goto cleanup;
        }
        pending = total - complete_bytes;
        if (pending != 0) {
            memmove(buffer, buffer + complete_bytes, pending);
        }
    }

cleanup:
    if (pcm != NULL) {
        if (stop_requested) {
            (void)snd_pcm_drop(pcm);
        }
        snd_pcm_close(pcm);
    }
    free(buffer);
    return status;
}

static int create_listener(const config_t *config) {
    char port[8];
    struct addrinfo hints;
    struct addrinfo *addresses = NULL;
    struct addrinfo *address;
    int listener = -1;
    int yes = 1;
    int result;

    (void)snprintf(port, sizeof(port), "%u", config->listen_port);
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_flags = AI_NUMERICSERV;
    result = getaddrinfo(config->listen_host, port, &hints, &addresses);
    if (result != 0) {
        log_message("ERROR", "Cannot resolve listen_host '%s': %s", config->listen_host,
                    gai_strerror(result));
        return -1;
    }
    for (address = addresses; address != NULL; address = address->ai_next) {
        listener = socket(address->ai_family, address->ai_socktype, address->ai_protocol);
        if (listener < 0) {
            continue;
        }
        (void)setsockopt(listener, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));
        if (bind(listener, address->ai_addr, address->ai_addrlen) == 0 && listen(listener, 4) == 0) {
            break;
        }
        (void)close(listener);
        listener = -1;
    }
    freeaddrinfo(addresses);
    if (listener < 0) {
        log_message("ERROR", "Cannot bind TCP %s:%u: %s", config->listen_host,
                    config->listen_port, strerror(errno));
    }
    return listener;
}

static void print_usage(const char *program) {
    fprintf(stderr, "Usage: %s [-c /path/to/config.json]\n", program);
}

int main(int argc, char **argv) {
    const char *config_path = DEFAULT_CONFIG_PATH;
    config_t config;
    int listener;

    if (argc == 3 && strcmp(argv[1], "-c") == 0) {
        config_path = argv[2];
    } else if (argc != 1) {
        print_usage(argv[0]);
        return 2;
    }
    if (parse_config(config_path, &config) != 0) {
        fprintf(stderr, "lp10-netaudio: invalid or unreadable config %s: %s\n", config_path,
                strerror(errno));
        return 1;
    }
    if (!direct_hw_device(config.alsa_device)) {
        fprintf(stderr, "lp10-netaudio: alsa_device must be a direct hw:X,Y target, not %s\n",
                config.alsa_device);
        return 1;
    }
    log_fp = fopen(config.log_file, "a");
    if (log_fp == NULL) {
        fprintf(stderr, "lp10-netaudio: cannot open log %s: %s\n", config.log_file, strerror(errno));
        return 1;
    }
    (void)setvbuf(log_fp, NULL, _IOLBF, 0);
    if (install_signal_handlers() != 0) {
        log_message("ERROR", "Cannot install signal handlers: %s", strerror(errno));
        (void)fclose(log_fp);
        return 1;
    }

    listener = create_listener(&config);
    if (listener < 0) {
        (void)fclose(log_fp);
        return 1;
    }
    log_message("INFO", "lp10-netaudio started: listening on %s:%u; ALSA device=%s",
                config.listen_host, config.listen_port, config.alsa_device);

    while (!stop_requested) {
        int client = accept(listener, NULL, NULL);
        if (client < 0) {
            if (errno == EINTR && !stop_requested) {
                continue;
            }
            if (!stop_requested) {
                log_message("WARN", "accept failed: %s", strerror(errno));
            }
            continue;
        }
        (void)handle_client(client, &config);
        (void)close(client);
    }
    (void)close(listener);
    log_message("INFO", "lp10-netaudio stopped");
    (void)fclose(log_fp);
    return 0;
}
