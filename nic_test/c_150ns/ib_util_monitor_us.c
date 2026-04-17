#define _GNU_SOURCE

#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <getopt.h>
#include <inttypes.h>
#include <limits.h>
#include <stdbool.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

#define BITS_PER_BYTE 8.0
#define BYTES_PER_DWORD 4.0
#define NS_PER_US 1000.0
#define NS_PER_SECOND 1000000000.0
#define DEFAULT_INTERVAL_US 100.0
#define DEFAULT_FLUSH_EVERY 1000ULL
#define DEFAULT_PRINT_EVERY 0ULL

typedef struct {
    char device[128];
    int port;
    double interval_us;
    char output_path[PATH_MAX];
    bool append;
    uint64_t count;
    bool count_set;
    bool list_ports;
    bool quiet;
    uint64_t flush_every;
    uint64_t print_every;
} Config;

typedef struct {
    char rate_text[128];
    char state[64];
    char phys_state[64];
    char link_layer[64];
    double rate_bps;
    int fd_xmit;
    int fd_rcv;
} Monitor;

typedef struct {
    uint64_t monotonic_raw_ns;
    uint64_t xmit_dwords;
    uint64_t rcv_dwords;
    uint64_t read_cost_ns;
} Snapshot;

typedef struct {
    uint64_t samples;
    long double total_interval_ns;
    long double total_read_cost_ns;
    uint64_t min_interval_ns;
    uint64_t max_interval_ns;
    double max_summary_util_pct;
} Stats;

static volatile sig_atomic_t g_stop_requested = 0;

static void handle_signal(int signum) {
    (void)signum;
    g_stop_requested = 1;
}

static void usage(FILE *stream, const char *prog) {
    fprintf(stream,
        "用法: %s [选项]\n"
        "\n"
        "选项:\n"
        "  --device NAME           RDMA 设备名，例如 mlx5_0 或 mlx5_bond_0\n"
        "  --port NUM              端口号，例如 1\n"
        "  --interval-us NUM       目标采样周期，单位 us，默认 %.1f\n"
        "  -o, --output PATH       输出 CSV 文件，默认 ib_utilization_us.csv\n"
        "  --append                追加写入已有文件\n"
        "  --count NUM             采样次数上限\n"
        "  --flush-every NUM       每写入 NUM 条记录 flush 一次，默认 %" PRIu64 "\n"
        "  --print-every NUM       每采样 NUM 次打印一次摘要，默认 %" PRIu64 " 表示不打印\n"
        "  --quiet                 关闭启动与结束摘要输出\n"
        "  --list-ports            列出当前主机可见的 RDMA 端口并退出\n"
        "  -h, --help              显示帮助\n",
        prog,
        DEFAULT_INTERVAL_US,
        (uint64_t)DEFAULT_FLUSH_EVERY,
        (uint64_t)DEFAULT_PRINT_EVERY
    );
}

static uint64_t get_monotonic_raw_ns(void) {
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC_RAW, &ts) != 0) {
        perror("clock_gettime(CLOCK_MONOTONIC_RAW)");
        exit(1);
    }
    return (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
}

static void trim_inplace(char *text) {
    size_t len = strlen(text);
    while (len > 0 && isspace((unsigned char)text[len - 1])) {
        text[--len] = '\0';
    }
    char *start = text;
    while (*start != '\0' && isspace((unsigned char)*start)) {
        start++;
    }
    if (start != text) {
        memmove(text, start, strlen(start) + 1);
    }
}

static void normalize_state_inplace(char *text) {
    trim_inplace(text);
    char *colon = strchr(text, ':');
    if (colon != NULL) {
        memmove(text, colon + 1, strlen(colon + 1) + 1);
        trim_inplace(text);
    }
}

static int read_text_file(const char *path, char *buffer, size_t capacity) {
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) {
        return -1;
    }
    ssize_t nread = read(fd, buffer, capacity - 1);
    close(fd);
    if (nread < 0) {
        return -1;
    }
    buffer[nread] = '\0';
    trim_inplace(buffer);
    return 0;
}

static int open_counter_file(const char *path) {
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) {
        fprintf(stderr, "错误: 无法打开计数器文件 %s: %s\n", path, strerror(errno));
        exit(1);
    }
    return fd;
}

static uint64_t read_counter_fd(int fd) {
    char buffer[64];
    ssize_t nread = pread(fd, buffer, sizeof(buffer) - 1, 0);
    if (nread < 0) {
        fprintf(stderr, "错误: 读取计数器失败: %s\n", strerror(errno));
        exit(1);
    }
    buffer[nread] = '\0';
    trim_inplace(buffer);
    errno = 0;
    char *end = NULL;
    unsigned long long value = strtoull(buffer, &end, 10);
    if (errno != 0 || end == buffer) {
        fprintf(stderr, "错误: 计数器内容不是有效整数: %s\n", buffer);
        exit(1);
    }
    return (uint64_t)value;
}

static double parse_rate_bps(const char *rate_text) {
    double value = 0.0;
    char unit = '\0';
    if (sscanf(rate_text, "%lf %c", &value, &unit) != 2) {
        fprintf(stderr, "错误: 无法解析端口速率文本: %s\n", rate_text);
        exit(1);
    }
    switch ((char)toupper((unsigned char)unit)) {
        case 'K':
            return value * 1e3;
        case 'M':
            return value * 1e6;
        case 'G':
            return value * 1e9;
        case 'T':
            return value * 1e12;
        default:
            fprintf(stderr, "错误: 未识别的速率单位: %c\n", unit);
            exit(1);
    }
}

static bool is_numeric_name(const char *name) {
    if (name == NULL || *name == '\0') {
        return false;
    }
    for (const char *p = name; *p != '\0'; ++p) {
        if (!isdigit((unsigned char)*p)) {
            return false;
        }
    }
    return true;
}

static void append_checked(char *dst, size_t capacity, size_t *offset, const char *text) {
    size_t text_len = strlen(text);
    if (*offset + text_len + 1 > capacity) {
        fprintf(stderr, "错误: 路径或标签过长，超出内部缓冲区上限。\n");
        exit(1);
    }
    memcpy(dst + *offset, text, text_len);
    *offset += text_len;
    dst[*offset] = '\0';
}

static void append_char_checked(char *dst, size_t capacity, size_t *offset, char ch) {
    if (*offset + 2 > capacity) {
        fprintf(stderr, "错误: 路径或标签过长，超出内部缓冲区上限。\n");
        exit(1);
    }
    dst[*offset] = ch;
    *offset += 1;
    dst[*offset] = '\0';
}

static void build_joined_path(char *dst, size_t capacity, const char *base, const char *child) {
    size_t offset = 0;
    dst[0] = '\0';
    append_checked(dst, capacity, &offset, base);
    append_char_checked(dst, capacity, &offset, '/');
    append_checked(dst, capacity, &offset, child);
}

static void build_suffixed_path(char *dst, size_t capacity, const char *base, const char *suffix) {
    size_t offset = 0;
    dst[0] = '\0';
    append_checked(dst, capacity, &offset, base);
    append_checked(dst, capacity, &offset, suffix);
}

static void build_device_port_label(char *dst, size_t capacity, const char *device, const char *port) {
    size_t offset = 0;
    dst[0] = '\0';
    append_checked(dst, capacity, &offset, device);
    append_char_checked(dst, capacity, &offset, ':');
    append_checked(dst, capacity, &offset, port);
}

static void build_joined_suffixed_path(
    char *dst,
    size_t capacity,
    const char *base,
    const char *child,
    const char *suffix
) {
    size_t offset = 0;
    dst[0] = '\0';
    append_checked(dst, capacity, &offset, base);
    append_char_checked(dst, capacity, &offset, '/');
    append_checked(dst, capacity, &offset, child);
    append_checked(dst, capacity, &offset, suffix);
}

static void join_first_netdev(const char *port_dir, char *buffer, size_t capacity) {
    char ndevs_dir[PATH_MAX];
    build_suffixed_path(ndevs_dir, sizeof(ndevs_dir), port_dir, "/gid_attrs/ndevs");
    DIR *dir = opendir(ndevs_dir);
    if (dir == NULL) {
        snprintf(buffer, capacity, "-");
        return;
    }

    struct dirent *entry;
    char path[PATH_MAX];
    char value[128];
    while ((entry = readdir(dir)) != NULL) {
        if (!is_numeric_name(entry->d_name)) {
            continue;
        }
        build_joined_path(path, sizeof(path), ndevs_dir, entry->d_name);
        if (read_text_file(path, value, sizeof(value)) == 0 && value[0] != '\0') {
            snprintf(buffer, capacity, "%s", value);
            closedir(dir);
            return;
        }
    }
    closedir(dir);
    snprintf(buffer, capacity, "-");
}

static void list_ports(void) {
    const char *base_dir = "/sys/class/infiniband";
    DIR *device_dir = opendir(base_dir);
    if (device_dir == NULL) {
        fprintf(stderr, "错误: 无法打开 %s: %s\n", base_dir, strerror(errno));
        exit(1);
    }

    printf("%-16s %-12s %-12s %-12s %-24s %-16s\n",
           "DEVICE:PORT", "LINK_LAYER", "STATE", "PHYS_STATE", "RATE", "NETDEV");
    printf("-------------------------------------------------------------------------------------------------\n");

    struct dirent *device_entry;
    while ((device_entry = readdir(device_dir)) != NULL) {
        if (device_entry->d_name[0] == '.') {
            continue;
        }
        char ports_dir[PATH_MAX];
        build_joined_suffixed_path(ports_dir, sizeof(ports_dir), base_dir, device_entry->d_name, "/ports");
        DIR *ports = opendir(ports_dir);
        if (ports == NULL) {
            continue;
        }

        struct dirent *port_entry;
        while ((port_entry = readdir(ports)) != NULL) {
            if (!is_numeric_name(port_entry->d_name)) {
                continue;
            }

            char port_dir[PATH_MAX];
            char link_layer[64] = "-";
            char state[64] = "-";
            char phys_state[64] = "-";
            char rate[128] = "-";
            char netdev[128] = "-";
            char path[PATH_MAX];

            build_joined_path(port_dir, sizeof(port_dir), ports_dir, port_entry->d_name);

            build_suffixed_path(path, sizeof(path), port_dir, "/link_layer");
            if (read_text_file(path, link_layer, sizeof(link_layer)) != 0) {
                snprintf(link_layer, sizeof(link_layer), "-");
            }
            build_suffixed_path(path, sizeof(path), port_dir, "/state");
            if (read_text_file(path, state, sizeof(state)) == 0) {
                normalize_state_inplace(state);
            }
            build_suffixed_path(path, sizeof(path), port_dir, "/phys_state");
            if (read_text_file(path, phys_state, sizeof(phys_state)) == 0) {
                normalize_state_inplace(phys_state);
            }
            build_suffixed_path(path, sizeof(path), port_dir, "/rate");
            if (read_text_file(path, rate, sizeof(rate)) != 0) {
                snprintf(rate, sizeof(rate), "-");
            }
            join_first_netdev(port_dir, netdev, sizeof(netdev));

            char label[PATH_MAX];
            build_device_port_label(label, sizeof(label), device_entry->d_name, port_entry->d_name);
            printf("%-16s %-12s %-12s %-12s %-24s %-16s\n",
                   label, link_layer, state, phys_state, rate, netdev);
        }
        closedir(ports);
    }
    closedir(device_dir);
}

static void config_init(Config *cfg) {
    memset(cfg, 0, sizeof(*cfg));
    cfg->interval_us = DEFAULT_INTERVAL_US;
    snprintf(cfg->output_path, sizeof(cfg->output_path), "ib_utilization_us.csv");
    cfg->flush_every = DEFAULT_FLUSH_EVERY;
    cfg->print_every = DEFAULT_PRINT_EVERY;
    cfg->port = -1;
}

static uint64_t parse_u64(const char *text, const char *name) {
    errno = 0;
    char *end = NULL;
    unsigned long long value = strtoull(text, &end, 10);
    if (errno != 0 || end == text || *end != '\0') {
        fprintf(stderr, "错误: %s 不是有效的无符号整数: %s\n", name, text);
        exit(1);
    }
    return (uint64_t)value;
}

static double parse_double_strict(const char *text, const char *name) {
    errno = 0;
    char *end = NULL;
    double value = strtod(text, &end);
    if (errno != 0 || end == text || *end != '\0') {
        fprintf(stderr, "错误: %s 不是有效浮点数: %s\n", name, text);
        exit(1);
    }
    return value;
}

static void parse_args(int argc, char **argv, Config *cfg) {
    static const struct option options[] = {
        {"device", required_argument, NULL, 1},
        {"port", required_argument, NULL, 2},
        {"interval-us", required_argument, NULL, 3},
        {"output", required_argument, NULL, 'o'},
        {"append", no_argument, NULL, 4},
        {"count", required_argument, NULL, 5},
        {"flush-every", required_argument, NULL, 6},
        {"print-every", required_argument, NULL, 7},
        {"quiet", no_argument, NULL, 8},
        {"list-ports", no_argument, NULL, 9},
        {"help", no_argument, NULL, 'h'},
        {0, 0, 0, 0}
    };

    int opt;
    int option_index = 0;
    while ((opt = getopt_long(argc, argv, "ho:", options, &option_index)) != -1) {
        switch (opt) {
            case 1:
                snprintf(cfg->device, sizeof(cfg->device), "%s", optarg);
                break;
            case 2:
                cfg->port = (int)parse_u64(optarg, "--port");
                break;
            case 3:
                cfg->interval_us = parse_double_strict(optarg, "--interval-us");
                break;
            case 'o':
                snprintf(cfg->output_path, sizeof(cfg->output_path), "%s", optarg);
                break;
            case 4:
                cfg->append = true;
                break;
            case 5:
                cfg->count = parse_u64(optarg, "--count");
                cfg->count_set = true;
                break;
            case 6:
                cfg->flush_every = parse_u64(optarg, "--flush-every");
                break;
            case 7:
                cfg->print_every = parse_u64(optarg, "--print-every");
                break;
            case 8:
                cfg->quiet = true;
                break;
            case 9:
                cfg->list_ports = true;
                break;
            case 'h':
                usage(stdout, argv[0]);
                exit(0);
            default:
                usage(stderr, argv[0]);
                exit(1);
        }
    }

    if (cfg->list_ports) {
        return;
    }
    if (optind < argc) {
        fprintf(stderr, "错误: 存在未识别的位置参数。\n");
        usage(stderr, argv[0]);
        exit(1);
    }
    if (cfg->device[0] == '\0' || cfg->port < 0) {
        fprintf(stderr, "错误: 运行采样时必须同时指定 --device 与 --port。\n");
        usage(stderr, argv[0]);
        exit(1);
    }
    if (cfg->interval_us <= 0.0) {
        fprintf(stderr, "错误: --interval-us 必须大于 0。\n");
        exit(1);
    }
    if (cfg->flush_every == 0) {
        fprintf(stderr, "错误: --flush-every 必须大于 0。\n");
        exit(1);
    }
}

static void monitor_init(const Config *cfg, Monitor *mon) {
    memset(mon, 0, sizeof(*mon));
    mon->fd_xmit = -1;
    mon->fd_rcv = -1;
    char base[PATH_MAX];
    char path[PATH_MAX];

    snprintf(base, sizeof(base), "/sys/class/infiniband/%s/ports/%d", cfg->device, cfg->port);

    build_suffixed_path(path, sizeof(path), base, "/link_layer");
    if (read_text_file(path, mon->link_layer, sizeof(mon->link_layer)) != 0) {
        fprintf(stderr, "错误: 无法读取 %s\n", path);
        exit(1);
    }
    if (strcasecmp(mon->link_layer, "InfiniBand") != 0 &&
        strcasecmp(mon->link_layer, "Ethernet") != 0) {
        fprintf(stderr,
                "错误: 端口 %s:%d 的 link_layer 为 %s；当前仅支持 InfiniBand 或 Ethernet(RoCE) 模式。\n",
                cfg->device, cfg->port, mon->link_layer);
        exit(1);
    }

    build_suffixed_path(path, sizeof(path), base, "/state");
    if (read_text_file(path, mon->state, sizeof(mon->state)) == 0) {
        normalize_state_inplace(mon->state);
    } else {
        snprintf(mon->state, sizeof(mon->state), "UNKNOWN");
    }

    build_suffixed_path(path, sizeof(path), base, "/phys_state");
    if (read_text_file(path, mon->phys_state, sizeof(mon->phys_state)) == 0) {
        normalize_state_inplace(mon->phys_state);
    } else {
        snprintf(mon->phys_state, sizeof(mon->phys_state), "UNKNOWN");
    }

    build_suffixed_path(path, sizeof(path), base, "/rate");
    if (read_text_file(path, mon->rate_text, sizeof(mon->rate_text)) != 0) {
        fprintf(stderr, "错误: 无法读取 %s\n", path);
        exit(1);
    }
    mon->rate_bps = parse_rate_bps(mon->rate_text);

    build_suffixed_path(path, sizeof(path), base, "/counters/port_xmit_data");
    mon->fd_xmit = open_counter_file(path);
    build_suffixed_path(path, sizeof(path), base, "/counters/port_rcv_data");
    mon->fd_rcv = open_counter_file(path);
}

static void monitor_close(Monitor *mon) {
    if (mon->fd_xmit >= 0) {
        close(mon->fd_xmit);
        mon->fd_xmit = -1;
    }
    if (mon->fd_rcv >= 0) {
        close(mon->fd_rcv);
        mon->fd_rcv = -1;
    }
}

static void take_snapshot(const Monitor *mon, Snapshot *snap) {
    uint64_t t0 = get_monotonic_raw_ns();
    uint64_t xmit = read_counter_fd(mon->fd_xmit);
    uint64_t rcv = read_counter_fd(mon->fd_rcv);
    uint64_t t1 = get_monotonic_raw_ns();

    snap->monotonic_raw_ns = t0 + (t1 - t0) / 2;
    snap->xmit_dwords = xmit;
    snap->rcv_dwords = rcv;
    snap->read_cost_ns = t1 - t0;
}

static void open_output_file(const Config *cfg, FILE **out_fp, bool *wrote_header) {
    const char *mode = cfg->append ? "a" : "w";
    *out_fp = fopen(cfg->output_path, mode);
    if (*out_fp == NULL) {
        fprintf(stderr, "错误: 无法打开输出文件 %s: %s\n", cfg->output_path, strerror(errno));
        exit(1);
    }
    setvbuf(*out_fp, NULL, _IOFBF, 1 << 20);

    bool need_header = true;
    if (cfg->append) {
        struct stat st;
        if (stat(cfg->output_path, &st) == 0 && st.st_size > 0) {
            need_header = false;
        }
    }

    if (need_header) {
        fprintf(*out_fp,
                "sample_index,monotonic_raw_ns,target_interval_us,interval_ns,interval_us,read_cost_ns,late_ns,"
                "device,port,rate_text,rate_bps,state,phys_state,link_layer,"
                "xmit_data_dwords,rcv_data_dwords,xmit_data_dwords_delta,rcv_data_dwords_delta,"
                "xmit_bytes_delta,rcv_bytes_delta,xmit_bps,rcv_bps,total_bps,"
                "xmit_util_pct,rcv_util_pct,aggregate_util_pct,summary_util_pct\n");
        *wrote_header = true;
    } else {
        *wrote_header = false;
    }
}

static void busy_wait_until(uint64_t deadline_ns) {
    while (!g_stop_requested && get_monotonic_raw_ns() < deadline_ns) {
    }
}

static void update_stats(Stats *stats, uint64_t interval_ns, uint64_t read_cost_ns, double summary_util_pct) {
    stats->samples += 1;
    stats->total_interval_ns += (long double)interval_ns;
    stats->total_read_cost_ns += (long double)read_cost_ns;
    if (stats->samples == 1 || interval_ns < stats->min_interval_ns) {
        stats->min_interval_ns = interval_ns;
    }
    if (interval_ns > stats->max_interval_ns) {
        stats->max_interval_ns = interval_ns;
    }
    if (summary_util_pct > stats->max_summary_util_pct) {
        stats->max_summary_util_pct = summary_util_pct;
    }
}

static void print_summary(const Config *cfg, const Monitor *mon, const Stats *stats) {
    if (cfg->quiet || stats->samples == 0) {
        return;
    }
    long double avg_interval_us = stats->total_interval_ns / (long double)stats->samples / NS_PER_US;
    long double avg_read_cost_us = stats->total_read_cost_ns / (long double)stats->samples / NS_PER_US;
    printf("监控结束: samples=%" PRIu64 ", avg_interval=%.3Lf us, min_interval=%.3f us, max_interval=%.3f us, "
           "avg_read_cost=%.3Lf us, max_summary_util=%.3f%%, port=%s:%d, rate=%s\n",
           stats->samples,
           avg_interval_us,
           stats->min_interval_ns / NS_PER_US,
           stats->max_interval_ns / NS_PER_US,
           avg_read_cost_us,
           stats->max_summary_util_pct,
           cfg->device,
           cfg->port,
           mon->rate_text);
}

int main(int argc, char **argv) {
    Config cfg;
    config_init(&cfg);
    parse_args(argc, argv, &cfg);

    if (cfg.list_ports) {
        list_ports();
        return 0;
    }

    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = handle_signal;
    sigaction(SIGINT, &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);

    Monitor mon;
    monitor_init(&cfg, &mon);

    FILE *out_fp = NULL;
    bool wrote_header = false;
    open_output_file(&cfg, &out_fp, &wrote_header);
    (void)wrote_header;

    if (!cfg.quiet) {
        printf("开始监控 %s:%d，输出文件: %s\n", cfg.device, cfg.port, cfg.output_path);
        printf("链路信息: rate=%s state=%s phys_state=%s link_layer=%s target_interval=%.3f us\n",
               mon.rate_text, mon.state, mon.phys_state, mon.link_layer, cfg.interval_us);
        printf("实现说明: 当前版本基于 RDMA 端口计数器 + CLOCK_MONOTONIC_RAW + busy wait；"
               "支持 InfiniBand 与 Ethernet(RoCE) 模式，实际间隔受 sysfs read 延迟限制。\n");
        printf("按 Ctrl+C 停止。\n");
    }

    Snapshot prev;
    take_snapshot(&mon, &prev);

    uint64_t interval_ns = (uint64_t)(cfg.interval_us * NS_PER_US);
    if (interval_ns == 0) {
        interval_ns = 1;
    }
    uint64_t next_deadline_ns = prev.monotonic_raw_ns + interval_ns;
    uint64_t sample_index = 0;
    Stats stats;
    memset(&stats, 0, sizeof(stats));

    while (!g_stop_requested) {
        if (cfg.count_set && sample_index >= cfg.count) {
            break;
        }

        busy_wait_until(next_deadline_ns);
        if (g_stop_requested) {
            break;
        }

        Snapshot cur;
        take_snapshot(&mon, &cur);

        uint64_t actual_interval_ns = cur.monotonic_raw_ns - prev.monotonic_raw_ns;
        uint64_t late_ns = (cur.monotonic_raw_ns > next_deadline_ns)
            ? (cur.monotonic_raw_ns - next_deadline_ns)
            : 0;
        uint64_t xmit_dwords_delta = cur.xmit_dwords - prev.xmit_dwords;
        uint64_t rcv_dwords_delta = cur.rcv_dwords - prev.rcv_dwords;
        uint64_t xmit_bytes_delta = (uint64_t)(xmit_dwords_delta * BYTES_PER_DWORD);
        uint64_t rcv_bytes_delta = (uint64_t)(rcv_dwords_delta * BYTES_PER_DWORD);

        double xmit_bps = 0.0;
        double rcv_bps = 0.0;
        if (actual_interval_ns > 0) {
            xmit_bps = (double)xmit_bytes_delta * BITS_PER_BYTE * NS_PER_SECOND / (double)actual_interval_ns;
            rcv_bps = (double)rcv_bytes_delta * BITS_PER_BYTE * NS_PER_SECOND / (double)actual_interval_ns;
        }
        double total_bps = xmit_bps + rcv_bps;
        double xmit_util_pct = (mon.rate_bps > 0.0) ? (xmit_bps / mon.rate_bps * 100.0) : 0.0;
        double rcv_util_pct = (mon.rate_bps > 0.0) ? (rcv_bps / mon.rate_bps * 100.0) : 0.0;
        double aggregate_util_pct = (mon.rate_bps > 0.0) ? (total_bps / (2.0 * mon.rate_bps) * 100.0) : 0.0;
        double summary_util_pct = (xmit_util_pct > rcv_util_pct) ? xmit_util_pct : rcv_util_pct;

        sample_index += 1;
        fprintf(out_fp,
                "%" PRIu64 ",%" PRIu64 ",%.3f,%" PRIu64 ",%.3f,%" PRIu64 ",%" PRIu64 ","
                "%s,%d,\"%s\",%.0f,%s,%s,%s,"
                "%" PRIu64 ",%" PRIu64 ",%" PRIu64 ",%" PRIu64 ","
                "%" PRIu64 ",%" PRIu64 ",%.3f,%.3f,%.3f,%.6f,%.6f,%.6f,%.6f\n",
                sample_index,
                cur.monotonic_raw_ns,
                cfg.interval_us,
                actual_interval_ns,
                actual_interval_ns / NS_PER_US,
                cur.read_cost_ns,
                late_ns,
                cfg.device,
                cfg.port,
                mon.rate_text,
                mon.rate_bps,
                mon.state,
                mon.phys_state,
                mon.link_layer,
                cur.xmit_dwords,
                cur.rcv_dwords,
                xmit_dwords_delta,
                rcv_dwords_delta,
                xmit_bytes_delta,
                rcv_bytes_delta,
                xmit_bps,
                rcv_bps,
                total_bps,
                xmit_util_pct,
                rcv_util_pct,
                aggregate_util_pct,
                summary_util_pct);

        if (cfg.flush_every > 0 && (sample_index % cfg.flush_every) == 0) {
            fflush(out_fp);
        }
        if (!cfg.quiet && cfg.print_every > 0 && (sample_index % cfg.print_every) == 0) {
            printf("[sample=%" PRIu64 "] interval=%.3f us read_cost=%.3f us late=%.3f us "
                   "tx=%.3f Gb/s (%.3f%%) rx=%.3f Gb/s (%.3f%%) agg=%.3f%% summary=%.3f%%\n",
                   sample_index,
                   actual_interval_ns / NS_PER_US,
                   cur.read_cost_ns / NS_PER_US,
                   late_ns / NS_PER_US,
                   xmit_bps / 1e9,
                   xmit_util_pct,
                   rcv_bps / 1e9,
                   rcv_util_pct,
                   aggregate_util_pct,
                   summary_util_pct);
        }

        update_stats(&stats, actual_interval_ns, cur.read_cost_ns, summary_util_pct);

        prev = cur;
        do {
            next_deadline_ns += interval_ns;
        } while (next_deadline_ns <= cur.monotonic_raw_ns);
    }

    fflush(out_fp);
    fclose(out_fp);
    print_summary(&cfg, &mon, &stats);
    monitor_close(&mon);
    return 0;
}
