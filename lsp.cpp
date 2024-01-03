#include <string>
#include <utility>
#include <memory>
#include <map>
#include <vector>
#include <thread>
#include <mutex>
#include <functional>
#include <poll.h>

#define TEST_SERVER "clangd"

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


static string uri_for_buffer(yed_buffer *buffer) {
    string uri = "";

    if (!(buffer->flags & BUFF_SPECIAL)
    &&  buffer->kind == BUFF_KIND_FILE) {
        if (buffer->path == NULL) {
            uri += "untitled:";
            uri += buffer->name;
        } else {
            uri += "file://";
            uri += buffer->path;
        }
    }

    return uri;
}

struct Message {
    json content;

    Message(json &&content) : content(content) {}
};

struct Notification : Message {
    Notification(json &&notification) : Message(std::move(notification)) {}
};

struct Request : Message {
    using Callback = function<void(Request&)>;

    request_id     id;
    string         method;
    optional<json> response;
    bool           error = false;
    Callback       on_response;

    Request(request_id id, json &&request, Callback &&on_response)
        : Message(std::move(request)), id(id), response(nullopt), on_response(on_response) {

        this->method = request["method"];
    }

    bool has_response()    { return this->response.has_value();                   }
    void handle_response() { if (this->on_response) { this->on_response(*this); } }
};

#define LAM_RQCB(...) ([this](Request &rq)        { __VA_ARGS__ ; })
#define LAM_SC(...)   ([](Server_Connection &con) { __VA_ARGS__ ; })

struct Edit {
    string new_content;
    int    start_line;
    int    start_char;
    int    end_line;
    int    end_char;

    Edit(string &&new_content, int start_line, int start_char, int end_line, int end_char)
        : new_content(std::move(new_content)),
          start_line(start_line),
          start_char(start_char),
          end_line(end_line),
          end_char(end_char)
        {}
};

struct Server_Connection {
    enum Server_State { CREATED, INITIALIZED, RUNNING, FAILED_TO_START, HANGUP };

    int                        rd_fd      = -1;
    int                        wr_fd      = -1;
    pid_t                      server_pid = -1;
    map<request_id, Request>   requests;
    vector<json>               notifications;
    vector<string>             problems;
    thread                     thr;
    mutex                      mtx;
    int                        sig_fds[2] = { -1, -1 };
    string                     name;
    string                     cmd;
    vector<string>             ft_names;
    vector<yed_buffer*>        managed_buffers;
    map<string, vector<Edit> > edit_queues;
    size_t                     version_counter = 0;
    Server_State               state = CREATED;

    Server_Connection() {}

    ~Server_Connection() {
        // Remove from ft_to_server map
again:;
        for (auto it = ft_to_server.begin(); it != ft_to_server.end();) {
            if (&(it->second) == this) {
                it = ft_to_server.erase(it);
                goto again;
            } else {
                ++it;
            }
        }

        if (this->state == INITIALIZED || this->state == RUNNING) {
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
        }
    }

    bool start() {
        if (this->state == INITIALIZED || this->state == RUNNING) {
            EDBG("the server is already started!");
            return false;
        } else if (this->state == FAILED_TO_START || this->state == HANGUP) {
            return false;
        }

        int fds_to_child[2];
        int fds_from_child[2];

        if (pipe(fds_to_child) == -1) {
            errno = 0;
            EDBG("failed to open write pipe");
            return false;
        }
        if (pipe(fds_from_child) == -1) {
            errno = 0;
            EDBG("failed to open read pipe");
            return false;
        }

        pid_t pid = fork();

        if (pid < 0) {
            this->state = FAILED_TO_START;
            errno = 0;
            close(fds_to_child[0]);   close(fds_to_child[1]);
            close(fds_from_child[0]); close(fds_from_child[1]);
            EDBG("failed to fork server");
            return false;
        }

        if (pid == 0) {
            while ((dup2(fds_to_child[0],   0) == -1) && (errno == EINTR)) {}
            while ((dup2(fds_from_child[1], 1) == -1) && (errno == EINTR)) {}

            /* clangd seems to have some issues when I close stderr for it... */
//             close(2);

            close(fds_to_child[0]);   close(fds_to_child[1]);
            close(fds_from_child[0]); close(fds_from_child[1]);
            execl("/bin/sh", "sh", "-c", this->cmd.c_str(), NULL);
            exit(99);
        }

        close(fds_to_child[0]);
        close(fds_from_child[1]);

        this->rd_fd      = fds_from_child[0];
        this->wr_fd      = fds_to_child[1];
        this->server_pid = pid;

        int flags = fcntl(this->rd_fd, F_GETFL);
        int err   = fcntl(this->rd_fd, F_SETFL, flags | O_NONBLOCK);
        (void)err;

        if (pipe(this->sig_fds) != 0) {
            this->state = FAILED_TO_START;
            errno = 0;
            EDBG("failed to open thread signal pipe");
            return false;
        }

        this->thr = thread(Server_Connection::pull_thread, this);

        this->state = INITIALIZED;

        this->send_initialize();

        return true;
    }

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

        string res;

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

            /* Check if the pipe has closed or a pipe error has occurred. */
            if (!(pfds[0].revents & POLLIN)) {
                lock_guard<mutex> lck(con.mtx);
                con.problems.emplace_back("server hangup:" + con.cmd);
                con.state = HANGUP;
                return;
            }

keep_reading:;
            while ((n = read(con.rd_fd, buff, sizeof(buff) - 1)) > 0) {
                buff[n] = 0;
                res.append(buff);
            }

            int e = 0;
            if (n < 0) {
                e     = errno;
                errno = 0;

                switch (e) {
                    case EWOULDBLOCK:
                        /* Continue below. We should have stuff in res. */
                        break;
                    case EINTR:
                        goto keep_reading;
                    default:
                        lock_guard<mutex> lck(con.mtx);
                        char pbuff[128];
                        snprintf(pbuff, sizeof(pbuff), "read(): errno: %d", e);
                        con.problems.emplace_back(pbuff);
                        return;
                }
            }

            if (res.size()) {
                string content = std::move(res);
                res            = string();

                size_t len;
                if (sscanf(content.c_str(), "Content-Length: %zu", &len) != 1) {
                    lock_guard<mutex> lck(con.mtx);

                    con.problems.emplace_back("unable to parse Content-Length");
                    continue;
                }

                auto split = content.find("\r\n\r\n");
                if (split == string::npos) { continue; }

                auto header = content.substr(0, split + 4);

                content.erase(content.begin(), content.begin() + header.size());

                if (content.size() < len) {
                    res += header;
                    res += content;
                    continue;
                }

                res += content.substr(len);
                content.erase(content.begin() + len, content.end());

                try {
                    json response = json::parse(content);

                    lock_guard<mutex> lck(con.mtx);

                    if (response.contains("id")) {
                        auto rqit = con.requests.find(response["id"]);
                        if (rqit != con.requests.end()) {
                            rqit->second.response = response;
                        }
                    } else {
                        con.notifications.push_back(json::parse(content));
                    }
                } catch (exception &e) {
                    lock_guard<mutex> lck(con.mtx);

                    con.problems.emplace_back(e.what());
                    continue;
                }

                yed_force_update();
            }

            if (res.size()) {
                goto keep_reading;
            }
        }
    }

    bool message(const Message &message) {
        const auto &j = message.content;

        auto bytes = j.dump();

        ssize_t write_ret;
        char header_buff[128];
        snprintf(header_buff, sizeof(header_buff), "Content-Length: %d\r\n\r\n", (int)bytes.size());

        bytes = string(header_buff) + bytes;

        write_ret = write(this->wr_fd, bytes.data(), bytes.size());
        if (write_ret < 0) {
            EDBG("failed to write to server (errno=%d, fd=%d)", errno, this->wr_fd);
            return false;
        }
        return true;
    }

    void notify(const string &&method, const json&& params) {
        Notification notif({{ "jsonrpc", "2.0" }, { "method", method }, { "params", params }});

        this->message(notif);
    }

    void notify(const string &&method) {
        this->notify(std::move(method), {});
    }

    optional<request_id> request(const string &&method, const json&& params, Request::Callback &&callback) {
        auto id = id_counter++;

        Request rq(id,
                   {{ "jsonrpc", "2.0" }, { "id", id}, { "method", method }, { "params", params }},
                   std::move(callback));

        if (!this->message(rq)) {
            return nullopt;
        }

        this->requests.emplace(id, std::move(rq));

        return id;
    }

    optional<request_id> request(const string &&method, Request::Callback &&callback) {
        return this->request(std::move(method), {}, std::move(callback));
    }

    void send_initialized() {
        this->notify("initialized");
    }

    void send_opened_buffer(yed_buffer *buffer) {
        if (buffer->flags & BUFF_SPECIAL
        ||  buffer->kind != BUFF_KIND_FILE) {

            return;
        }

        string uri = uri_for_buffer(buffer);

        char *buffer_contents = yed_get_buffer_text(buffer);
        string text = buffer_contents;
        free(buffer_contents);

        const char *ft_name = yed_get_ft_name(buffer->ft);
        string langid = ft_name == NULL ? "unknown" : ft_name;

        this->notify("textDocument/didOpen",
            {
                { "textDocument", {
                    { "uri",        uri    },
                    { "languageId", langid },
                    { "version",    0      },
                    { "text",       text   },
                }},
            }
        );
    }

    void send_closed_buffer(yed_buffer *buffer) {
        if (buffer->flags & BUFF_SPECIAL
        ||  buffer->kind != BUFF_KIND_FILE) {

            return;
        }

        string uri = uri_for_buffer(buffer);

        this->notify("textDocument/didClose",
            {
                { "textDocument", {
                    { "uri", uri },
                }},
            }
        );
    }

    void send_write_buffer(yed_buffer *buffer) {
        if (buffer->flags & BUFF_SPECIAL
        ||  buffer->kind != BUFF_KIND_FILE) {

            return;
        }

        string uri = uri_for_buffer(buffer);

        char *buffer_contents = yed_get_buffer_text(buffer);
        string text = buffer_contents;
        free(buffer_contents);

        this->notify("textDocument/didSave",
            {
                { "textDocument", {
                    { "uri",  uri  },
                    { "text", text },
                }},
            }
        );
    }

    void send_buffer_edits(const string &uri, vector<Edit> &&edits) {
        vector<json> changes;

        for (const auto &edit : edits) {
            if (edit.start_line == -1) {
                json change = {
                    { "text", std::move(edit.new_content) }
                };
                changes.push_back(std::move(change));
            } else {
                json change = {
                    { "range",
                        {
                            { "start", { { "line", edit.start_line }, { "character", edit.start_char }, } },
                            { "end",   { { "line", edit.end_line   }, { "character", edit.end_char   }, } },
                        }
                    },
                    { "text", std::move(edit.new_content) },
                };
                changes.push_back(std::move(change));
            }
        }

        this->notify("textDocument/didChange",
            {
                { "textDocument",
                    {
                        { "uri",     uri                     },
                        { "version", this->version_counter++ },
                    }
                },
                { "contentChanges", std::move(changes) },
            }
        );
    }

    void finish_initialization() {
        this->send_initialized();

        this->state = RUNNING;

        DBG("the server has been initialized!");

        tree_it(yed_buffer_name_t, yed_buffer_ptr_t) it;
        tree_traverse(ys->buffers, it) {
            const char *ft_name = yed_get_ft_name(tree_it_val(it)->ft);
            if (ft_name == NULL) { continue; }
            if (find(this->ft_names.begin(), this->ft_names.end(), ft_name) != this->ft_names.end()) {
                this->manage_buffer(tree_it_val(it));
            }
        }
    }

    optional<request_id> send_initialize() {
        return this->request("initialize",

            // Params
            {{ "processId",    getpid()                            },
             { "capabilities", json::object()                      },
             { "rootUri",      string("file://") + ys->working_dir }
            },

            // Callback
            LAM_RQCB(
                if (rq.error) {
                    EDBG("initialize request failed");
                } else {
                    this->finish_initialization();
                }
            ));
    }

    bool is_managed_buffer(yed_buffer *buffer) {
        auto search = find(this->managed_buffers.begin(), this->managed_buffers.end(), buffer);
        return search != this->managed_buffers.end();
    }

    void unmanage_buffer(yed_buffer *buffer) {
        if (this->state != RUNNING) { return; }

        auto search = find(this->managed_buffers.begin(), this->managed_buffers.end(), buffer);
        if (search == this->managed_buffers.end()) {
            return;
        }
        this->send_closed_buffer(buffer);
        this->managed_buffers.erase(search);
    }

    void manage_buffer(yed_buffer *buffer) {
        if (this->state != RUNNING) { return; }

        this->unmanage_buffer(buffer);
        this->managed_buffers.push_back(buffer);
        this->send_opened_buffer(buffer);

        DBG("SERVER(%s): managing buffer '%s'", this->name.c_str(), buffer->name);
    }

    void handle_buffer_write(yed_buffer *buffer) {
        if (this->state != RUNNING) { return; }

        if (!is_managed_buffer(buffer)) {
            this->manage_buffer(buffer);
        } else {
            this->send_write_buffer(buffer);
        }
    }

    void handle_buffer_mod(yed_buffer *buffer, yed_buff_mod_event event, int row) {
        int start_line;
        int start_char;
        int end_line;
        int end_char;

        if (this->state != RUNNING) { return; }

        if (buffer->flags & BUFF_SPECIAL
        ||  buffer->kind != BUFF_KIND_FILE) {

            return;
        }

        lock_guard<mutex> lck(this->mtx);

        switch (event) {
            case BUFF_MOD_APPEND_TO_LINE:
            case BUFF_MOD_POP_FROM_LINE:
            case BUFF_MOD_CLEAR_LINE:
            case BUFF_MOD_SET_LINE:
            case BUFF_MOD_INSERT_INTO_LINE:
            case BUFF_MOD_DELETE_FROM_LINE: {
                yed_line *line = yed_buff_get_line(buffer, row);
                if (line == NULL) { return; }

                start_line = row - 1;
                start_char = 0;
                end_line   = start_line + 1;
                end_char   = 0;

                array_zero_term(line->chars);

                string text = string((const char*)array_data(line->chars));
                text += "\n";

                this->edit_queues[uri_for_buffer(buffer)].emplace_back(
                    std::move(text),
                    start_line,
                    start_char,
                    end_line,
                    end_char);
                break;
            }

            case BUFF_MOD_ADD_LINE:
            case BUFF_MOD_INSERT_LINE:
                start_line = row - 1;
                start_char = 0;
                end_line   = start_line;
                end_char   = 0;

                this->edit_queues[uri_for_buffer(buffer)].emplace_back(
                    "\n",
                    start_line,
                    start_char,
                    end_line,
                    end_char);
                break;

            case BUFF_MOD_DELETE_LINE:
                start_line = row - 1;
                start_char = 0;
                end_line   = start_line + 1;
                end_char   = 0;

                this->edit_queues[uri_for_buffer(buffer)].emplace_back(
                    "",
                    start_line,
                    start_char,
                    end_line,
                    end_char);
                break;

            case BUFF_MOD_CLEAR:
                start_line = -1;
                start_char = -1;
                end_line   = -1;
                end_char   = -1;

                this->edit_queues[uri_for_buffer(buffer)].emplace_back(
                    "",
                    start_line,
                    start_char,
                    end_line,
                    end_char);
                break;

            default:
                break;
        }
    }
};

static thread::id                     main_thread_id;
static map<string, Server_Connection> servers;
static mutex                          servers_lock;

static Server_Connection * get_server(const string &name) {
    auto search = servers.find(name);
    if (search == servers.end()) {
        return nullptr;
    }

    return &(search->second);
}

static void del_server(const string &name) {
    const Server_Connection *conp = get_server(name);
    if (conp) {
        servers.erase(name);
    }
}

static Server_Connection * create_server(const string name, const string server_cmd, vector<string> ft_names) {
    Server_Connection &con = servers[name];

    con.name     = std::move(name);
    con.cmd      = std::move(server_cmd);
    con.ft_names = std::move(ft_names);

    return &con;
}

static void define_server(const string &name, const string &server_cmd, const vector<string> &ft_names) {
    Server_Connection *conp = get_server(name);

    if (conp) {
        conp = nullptr;
        del_server(name);
    }

    conp = create_server(name, server_cmd, ft_names);

    for (const auto &ft : conp->ft_names) {
        ft_to_server.insert(pair<string, Server_Connection&>(ft, *conp));
    }

    tree_it(yed_buffer_name_t, yed_buffer_ptr_t) it;
    tree_traverse(ys->buffers, it) {
        if (find(conp->ft_names.begin(), conp->ft_names.end(), tree_it_key(it)) != conp->ft_names.end()) {
            if (!conp->start()) {
                del_server(conp->name);
                return;
            }
            break;
        }
    }
}


static void broadcast(const std::string &method, const json &notif) {
    yed_event event;
    string    text;

    text = notif.dump();

    event.kind                       = EVENT_PLUGIN_MESSAGE;
    event.plugin_message.message_id  = method.c_str();
    event.plugin_message.plugin_id   = "lsp";
    event.plugin_message.string_data = text.c_str();

    yed_trigger_event(&event);
}

static void unload(yed_plugin *self) {
}

struct Save_Broadcast {
    std::string method;
    json        notif;

    Save_Broadcast(const std::string &_method, const json &&_notif)
        : method(_method), notif(std::move(_notif)) {}
};

static void pump(yed_event *event) {
    std::vector<Save_Broadcast> broadcasts;

    { lock_guard<mutex> lock(servers_lock);

        for (auto &serv : servers) {
            Server_Connection &con = serv.second;

            lock_guard<mutex> lck(con.mtx);

            for (const auto &p : con.problems) {
                EDBG("PROBLEM:\n%s", p.c_str());
            }
            con.problems.clear();

            for (const auto &n : con.notifications) {
                if (n.contains("method")) {
                    broadcasts.emplace_back(n["method"], std::move(n));
                }
            }
            con.notifications.clear();

            for (auto &r : con.requests) {
                Request &rq = r.second;

                rq.response.map([&](const json &js) {
                    if (js.contains("error")) {
                        EDBG("response error for request with id %lu", (unsigned long)rq.id);
                        EDBG("%s", js.dump(2).c_str());
                        rq.error = true;
                    }

                    if (rq.on_response) {
                        rq.handle_response();
                    } else {
                        broadcasts.emplace_back(rq.method, std::move(js));
                    }
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

    for (auto &b : broadcasts) {
        broadcast(b.method, b.notif);
    }
}

static void postpump(yed_event *event) {
    lock_guard<mutex> lock(servers_lock);

    for (auto &serv : servers) {
        Server_Connection &con = serv.second;

        lock_guard<mutex> lck(con.mtx);

        for (auto &it: con.edit_queues) {
            con.send_buffer_edits(it.first, std::move(it.second));
        }

        con.edit_queues.clear();
    }
}

static void buffer_event_server_action(yed_event *event, function<void(Server_Connection&, yed_event*)> action) {
    if (event->buffer != NULL && !(event->buffer->flags & BUFF_SPECIAL)) {
        lock_guard<mutex> lock(servers_lock);

        const char *ft_name = yed_get_ft_name(event->buffer->ft);
        if (ft_name == NULL) { return; }

        auto search = ft_to_server.find(ft_name);
        if (search != ft_to_server.end()) {
            Server_Connection &con = search->second;

            switch (con.state) {
                case Server_Connection::FAILED_TO_START:
                case Server_Connection::HANGUP:
                case Server_Connection::INITIALIZED: /* Not ready yet. */
                    return;
                case Server_Connection::CREATED:
                    if (!con.start()) {
                        del_server(con.name);
                        return;
                    }
                    break;
                case Server_Connection::RUNNING:
                    break;
            }
            action(con, event);
        }
    }
}

#define LAM_BUFFACT(...)   ([](Server_Connection &con, yed_event *event) { __VA_ARGS__ ; })

static void buffdel(yed_event *event) {
    buffer_event_server_action(event, LAM_BUFFACT(con.unmanage_buffer(event->buffer)));
}

static void presetft(yed_event *event) {
    buffer_event_server_action(event, LAM_BUFFACT(con.unmanage_buffer(event->buffer)));
}

static void postsetft(yed_event *event) {
    buffer_event_server_action(event, LAM_BUFFACT(con.manage_buffer(event->buffer)));
}

static void buffwrite(yed_event *event) {
    buffer_event_server_action(event, LAM_BUFFACT(con.handle_buffer_write(event->buffer)));
}

static void buffmod(yed_event *event) {
    buffer_event_server_action(event, LAM_BUFFACT(
        con.handle_buffer_mod(event->buffer, (yed_buff_mod_event)event->buff_mod_event, event->row);
    ));
}

static void pmsg(yed_event *event) {
    if (strcmp(event->plugin_message.plugin_id, "lsp") == 0) { return; }

    string prefix = "lsp-request:";
    if (strncmp(event->plugin_message.message_id, prefix.c_str(), prefix.size()) != 0) { return; }

    string method = event->plugin_message.message_id + prefix.size();

    const char *ft_name = yed_get_ft_name(event->ft);
    if (ft_name == NULL) { return; }

    auto search = ft_to_server.find(ft_name);
    if (search == ft_to_server.end()) { return; }

    Server_Connection &con = search->second;

    if (con.state == Server_Connection::RUNNING) {
        DBG("sending request on behalf of plugin");
        json params = json::parse(event->plugin_message.string_data);
        con.request(std::move(method), std::move(params), nullptr);
    }

    event->cancel = 1;
}

static void lsp_define_server(int n_args, char **args) {
    if (n_args < 2) {
        yed_cerr("expected at least 3 arguments, but got %d", n_args);
        return;
    }

    string name = args[0];
    string cmd  = args[1];

    vector<string> ft_strings;

    for (int i = 2; i < n_args; i += 1) {
        ft_strings.emplace_back(args[i]);
    }

    define_server(name, cmd, ft_strings);
}

extern "C"
int yed_plugin_boot(yed_plugin *self) {
    YED_PLUG_VERSION_CHECK();

    Self           = self;
    main_thread_id = this_thread::get_id();

    map<void(*)(yed_event*), vector<yed_event_kind_t> > event_handlers = {
        { pump,      { EVENT_PRE_PUMP           } },
        { postpump,  { EVENT_POST_PUMP          } },
        { buffdel,   { EVENT_BUFFER_PRE_DELETE  } },
        { presetft,  { EVENT_BUFFER_PRE_SET_FT  } },
        { postsetft, { EVENT_BUFFER_POST_SET_FT } },
        { buffwrite, { EVENT_BUFFER_POST_WRITE  } },
        { buffmod,   { EVENT_BUFFER_POST_MOD    } },
        { pmsg,      { EVENT_PLUGIN_MESSAGE     } },
    };

    map<const char*, const char*> vars = {
        { "lsp-debug-log", "OFF" },
    };

    map<const char*, void(*)(int, char**)> cmds = {
        { "lsp-define-server", lsp_define_server },
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

    return 0;
}
