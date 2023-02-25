#include <utility>
#include <memory>
#include <string>
#include <map>
#include <vector>
#include <thread>
#include <mutex>
#include <functional>
#include <poll.h>

using namespace std;

#include "json.hpp"
using json = nlohmann::json;

#include "optional.hpp"
using namespace tl;

extern "C" {
#include <yed/plugin.h>
}


#define DBG_LOG_ON

#define LOG__XSTR(x) #x
#define LOG_XSTR(x) LOG__XSTR(x)

#define LOG(...)                                                   \
do {                                                               \
    LOG_FN_ENTER();                                                \
    yed_log(__VA_ARGS__);                                          \
    LOG_EXIT();                                                    \
} while (0)

#define ELOG(...)                                                  \
do {                                                               \
    LOG_FN_ENTER();                                                \
    yed_log("[!] " __VA_ARGS__);                                   \
    LOG_EXIT();                                                    \
} while (0)

#ifdef DBG_LOG_ON
#define DBG(...)                                                   \
do {                                                               \
    if (yed_var_is_truthy("lsp-debug-log")) {                      \
        LOG_FN_ENTER();                                            \
        yed_log(__FILE__ ":" LOG_XSTR(__LINE__) ": " __VA_ARGS__); \
        LOG_EXIT();                                                \
    }                                                              \
} while (0)
#define EDBG(...)                                                  \
do {                                                               \
    if (yed_var_is_truthy("lsp-debug-log")) {                      \
        ELOG(__FILE__ ":" LOG_XSTR(__LINE__) ": " __VA_ARGS__);    \
    }                                                              \
} while (0)
#else
#define DBG(...) ;
#define EDBG(...) ;
#endif


typedef u64 request_id;

struct Server_Connection;

static yed_plugin                      *Self;
static request_id                       id_counter;
static map<string, Server_Connection&>  ft_to_server;

struct Request {
    using Callback = function<void(Request&)>;

    request_id     id;
    json           request;
    optional<json> response;
    bool           error = false;
    Callback       on_response;

    Request(request_id id, json &&request, Callback &&on_response)
        : id(id), request(request), response(nullopt), on_response(on_response) {}

    bool has_response()    { return this->response.has_value(); }
    void handle_response() { this->on_response(*this);          }
};

#define LAM_RQCB(...) ([](Request &rq)            { __VA_ARGS__ ; })
#define LAM_SC(...)   ([](Server_Connection &con) { __VA_ARGS__ ; })

struct Server_Connection {
    int                      rd_fd      = -1;
    int                      wr_fd      = -1;
    pid_t                    server_pid = -1;
    map<request_id, Request> requests;
    thread                   thr;
    mutex                    mtx;
    int                      sig_fds[2] = { -1, -1 };

    static optional<Server_Connection&> start(const string name, const string server_cmd);

    static void pull_thread(Server_Connection *_con) {
        Server_Connection &con = *_con;
        struct pollfd      pfds[2];
        ssize_t            n;
        char               buff[1024];

        pfds[0].fd      = con.rd_fd;
        pfds[0].events  = POLLIN;
        pfds[0].revents = 0;
        pfds[1].fd      = con.sig_fds[0];
        pfds[1].events  = POLLIN;
        pfds[1].revents = 0;

        for (;;) {
            if (poll(pfds, 2, -1) <= 0) {
                if (errno) {
                    if (errno != EINTR) { return; }
                    errno = 0;
                }
                continue;
            }

            errno = 0;

            /* Check if our connection is being closed and we've been signaled to stop. */
            if (pfds[1].revents & POLLIN) { return; }

            string res;

keep_reading:;
            while ((n = read(con.rd_fd, buff, sizeof(buff) - 1)) > 0) {
                buff[n] = 0;
                res.append(buff);
            }

            if (n < 0) {
                auto e = errno;
                errno  = 0;

                switch (e) {
                    case EWOULDBLOCK:
                        /* Continue below. We should have stuff in res. */
                        break;
                    case EINTR:
                        goto keep_reading;
                    default:
                        return;
                }
            }

            if (res.size()) {
                auto split = res.find("\r\n\r\n");
                if (split == string::npos) { continue; }

                res.erase(res.begin(), res.begin() + split + 4);

                try {
                    json response = json::parse(res);

                    if (response.contains("id")) {
                        lock_guard<mutex> lck(con.mtx);
                        auto rqit = con.requests.find(response["id"]);
                        if (rqit != con.requests.end()) {
                            rqit->second.response = response;
                        } else {
                            EDBG("not found");
                        }
                    }
                } catch (...) {
                    DBG("HERE");
                    // Ignore it I guess?
                    continue;
                }

                yed_force_update();
            }
        }
    }

    Server_Connection() {}

    ~Server_Connection() {
        // Remove from ft_to_server map
        for (auto it = ft_to_server.begin(); it != ft_to_server.end();) {
            if (&(it->second) == this) {
                it = ft_to_server.erase(it);
            } else {
                ++it;
            }
        }

        // Signal pull_thread to stop
        char z = 0;
        write(this->sig_fds[1], &z, 1);

        // Wait for it to stop
        this->thr.join();

        close(this->sig_fds[1]);
        close(this->sig_fds[0]);

        // Close server pipes and kill it
        if (this->rd_fd >= 0) {
            close(this->rd_fd);
            this->rd_fd = -1;
        }
        if (this->wr_fd >= 0) {
            close(this->wr_fd);
            this->wr_fd = -1;
        }
        if (this->server_pid >= 0) {
            kill(this->server_pid, SIGKILL);
        }

        DBG("closed LSP server connection");
    }

    optional<request_id> request(const string &&method, const json&& params, Request::Callback &&callback) {
        auto id = id_counter++;

        Request rq(id,
                   {{ "jsonrpc", "2.0" }, { "id", id}, { "method", method }, { "params", params }},
                   move(callback));

        const auto &j = rq.request;

        auto bytes = j.dump();

        DBG("requesting:\n%s", j.dump(2).c_str());

        ssize_t write_ret;
        char header_buff[128];
        snprintf(header_buff, sizeof(header_buff), "Content-Length: %d\r\n\r\n", (int)bytes.size());

        bytes = string(header_buff) + bytes;

        write_ret = write(this->wr_fd, bytes.data(), bytes.size());
        if (write_ret < 0) {
            EDBG("failed to write to server (errno=%d, fd=%d)", errno, this->wr_fd);
            return nullopt;
        }

        this->requests.emplace(id, move(rq));

        return id;
    }

    optional<request_id> request(const string &&method, Request::Callback &&callback) {
        return this->request(move(method), {}, move(callback));
    }

    optional<request_id> send_initialize() {
        return this->request("initialize",

            // Params
            {{ "processId",    to_string(getpid()) },
             { "capabilities", { }                 }},

            // Callback
            LAM_RQCB(if (rq.error) ELOG("initialize request failed")));
    }
};

static thread::id                     main_thread_id;
static map<string, Server_Connection> servers;
static mutex                          servers_lock;

static Server_Connection& get_server(const string name) {
    Server_Connection *sp;
    {
        lock_guard<mutex> lock(servers_lock);
        sp = &(servers[name]);
    }
    return *sp;
}

static void del_server(const string name) {
    if (this_thread::get_id() != main_thread_id) {
        ASSERT(0, "del_server called on non-main thread!");
        return;
    }

    {
        lock_guard<mutex> lock(servers_lock);
        servers.erase(name);
    }
}

optional<Server_Connection&> Server_Connection::start(const string name, const string server_cmd) {
    if (servers.find(name) != servers.end()) {
        servers.erase(name);
    }

    Server_Connection &con = get_server(name);

    int fds_to_child[2];
    int fds_from_child[2];

    if (pipe(fds_to_child) == -1) {
        errno = 0;
        EDBG("failed to open write pipe");
        return nullopt;
    }
    if (pipe(fds_from_child) == -1) {
        errno = 0;
        EDBG("failed to open read pipe");
        return nullopt;
    }

    pid_t pid = fork();

    if (pid < 0) {
        errno = 0;
        close(fds_to_child[0]);   close(fds_to_child[1]);
        close(fds_from_child[0]); close(fds_from_child[1]);
        EDBG("failed to fork server");
        return nullopt;
    }

    if (pid == 0) {
        while ((dup2(fds_to_child[0],   0) == -1) && (errno == EINTR)) {}
        while ((dup2(fds_from_child[1], 1) == -1) && (errno == EINTR)) {}
        close(2);
        close(fds_to_child[0]);   close(fds_to_child[1]);
        close(fds_from_child[0]); close(fds_from_child[1]);
        execl("/bin/sh", "sh", "-c", server_cmd.c_str(), NULL);
        exit(99);
    }

    close(fds_to_child[0]);
    close(fds_from_child[1]);

    con.rd_fd      = fds_from_child[0];
    con.wr_fd      = fds_to_child[1];
    con.server_pid = pid;

    int flags = fcntl(con.rd_fd, F_GETFL);
    int err   = fcntl(con.rd_fd, F_SETFL, flags | O_NONBLOCK);
    (void)err;

    if (pipe(con.sig_fds) != 0) {
        errno = 0;
        EDBG("failed to open thread signal pipe");
        return nullopt;
    }

    con.thr = thread(Server_Connection::pull_thread, &con);

    return con;
}

static void initialize_server_in_background(const string name, const string server_cmd) {
    thread t = thread([name, server_cmd] {
        auto con = Server_Connection::start(name, server_cmd);
        con.map(LAM_SC(con.send_initialize()));
    });
    t.detach();
}

static void unload(yed_plugin *self) {
}

static void pump(yed_event *event) {
    lock_guard<mutex> lock(servers_lock);

    for (auto &serv : servers) {
        Server_Connection &con = serv.second;

        lock_guard<mutex> lck(con.mtx);

        for (auto &r : con.requests) {
            Request &rq = r.second;

            rq.response.map([&](json &js) {
                if (js.contains("error")) {
                    ELOG("response error for request with id %lu", (unsigned long)rq.id);
                    rq.error = true;
                }

                yed_buffer *buff = yed_get_or_create_special_rdonly_buffer("*lsp-json-response");
                buff->flags &= ~BUFF_RD_ONLY;
                yed_buff_clear_no_undo(buff);
                yed_buff_insert_string_no_undo(buff, js.dump(2).c_str(), 1, 1);
                buff->flags |= BUFF_RD_ONLY;

                rq.handle_response();
            });
        }

        for (auto it = con.requests.begin(); it != con.requests.end();) {
            if (it->second.has_response()) {
                it = con.requests.erase(it);
            } else {
                ++it;
            }
        }
    }
}

extern "C"
int yed_plugin_boot(yed_plugin *self) {
    YED_PLUG_VERSION_CHECK();

    Self           = self;
    main_thread_id = this_thread::get_id();

//     yed_get_or_create_special_rdonly_buffer("*lsp-json-response");

    map<void(*)(yed_event*), vector<yed_event_kind_t> > event_handlers = {
        { pump,    { EVENT_PRE_PUMP } },
    };

    map<const char*, const char*> vars = {
        { "lsp-debug-log",              "ON"                        },
    };

    map<const char*, void(*)(int, char**)> cmds = {
//         { "term-new",           term_new_cmd           },
    };

    for (auto &pair : event_handlers) {
        for (auto evt : pair.second) {
            yed_event_handler h;
            h.kind = evt;
            h.fn   = pair.first;
            yed_plugin_add_event_handler(self, h);
        }
    }

    for (auto &pair : vars) {
        if (!yed_get_var(pair.first)) { yed_set_var(pair.first, pair.second); }
    }

    for (auto &pair : cmds) {
        yed_plugin_set_command(self, pair.first, pair.second);
    }

    yed_plugin_set_unload_fn(self, unload);

    initialize_server_in_background("clangd", "clangd");

    return 0;
}
