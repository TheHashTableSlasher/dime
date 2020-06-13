#include <assert.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <jansson.h>
#include "client.h"
#include "deque.h"
#include "server.h"
#include "socket.h"
#include "table.h"

int dime_client_init(dime_client_t *clnt, int fd) {
    clnt->fd = fd;

    clnt->groups_len = 0;
    clnt->groups_cap = 4;

    clnt->groups = malloc(sizeof(dime_group_t *) * clnt->groups_cap);
    if (clnt->groups == NULL) {
        return -1;
    }

    if (dime_socket_init(&clnt->sock, fd) < 0) {
        free(clnt->groups);

        return -1;
    }

    if (dime_deque_init(&clnt->queue) < 0) {
        dime_socket_destroy(&clnt->sock);
        free(clnt->groups);

        return -1;
    }

    return 0;
}

void dime_client_destroy(dime_client_t *clnt) {
    for (size_t i = 0; i < clnt->groups_len; i++) {
        dime_group_t *group = clnt->groups[i];

        for (size_t j = 0; j < group->clnts_len; j++) {
            if (group->clnts[j] == clnt) {
                group->clnts_len--;
                group->clnts[j] = group->clnts[group->clnts_len];

                break;
            }
        }
    }

    dime_deque_iter_t it;

    dime_deque_iter_init(&it, &clnt->queue);

    while (dime_deque_iter_next(&it)) {
        dime_rcmessage_t *msg = it.val;

        msg->refs--;

        if (msg->refs == 0) {
            json_decref(msg->jsondata);
            free(msg);
        }
    }

    free(clnt->groups);
    dime_deque_destroy(&clnt->queue);
    dime_socket_destroy(&clnt->sock);
}

int dime_client_register(dime_client_t *clnt, dime_server_t *srv, json_t *jsondata, void *bindata, size_t bindata_len) {
    const char *serialization;
    int serialization_i;

    if (json_unpack(jsondata, "{ss}", "serialization", &serialization) < 0) {
        return -1;
    }

    if (strcmp(serialization, "matlab") == 0) {
        serialization_i = DIME_MATLAB;
    } else if (strcmp(serialization, "pickle") == 0) {
        serialization_i = DIME_PICKLE;
    } else if (strcmp(serialization, "dimeb") == 0) {
        serialization_i = DIME_DIMEB;
    } else {
        return -1;
    }

    if (srv->serialization == DIME_NO_SERIALIZATION) {
        srv->serialization = serialization_i;
    } else if (srv->serialization != serialization_i) {
        if (srv->serialization != DIME_DIMEB) {
            json_t *meta = json_pack("{sbss}", "meta", 1, "serialization", "dimeb");
            if (meta == NULL) {
                return -1;
            }

            char *meta_str = json_dumps(meta, JSON_COMPACT);
            if (meta_str == NULL) {
                json_decref(meta);

                return -1;
            }

            json_decref(meta);

            dime_table_iter_t it;
            dime_table_iter_init(&it, &srv->name2clnt);

            while (dime_table_iter_next(&it)) {
                if (dime_socket_push_str(&clnt->sock, meta_str, NULL, 0) < 0) {
                    free(meta_str);

                    return -1;
                }
            }

            free(meta_str);
        }

        srv->serialization = DIME_DIMEB;
    }

    switch (srv->serialization) {
    case DIME_MATLAB:
        serialization = "matlab";
        break;

    case DIME_PICKLE:
        serialization = "pickle";
        break;

    case DIME_DIMEB:
        serialization = "dimeb";
        break;
    }

    json_t *response = json_pack("{siss}", "status", 0, "serialization", serialization);
    if (response == NULL) {
        return -1;
    }

    if (dime_socket_push(&clnt->sock, response, NULL, 0) < 0) {
        json_decref(response);

        return -1;
    }

    json_decref(response);

    return 0;
}

int dime_client_join(dime_client_t *clnt, dime_server_t *srv, json_t *jsondata, void *bindata, size_t bindata_len) {
    const char *name;

    if (json_unpack(jsondata, "{ss}", "name", &name) < 0) {
        return -1;
    }

    for (size_t i = 0; i < clnt->groups_len; i++) {
        if (strcmp(name, clnt->groups[i]->name) == 0) {
            return -1;
        }
    }

    dime_group_t *group = dime_table_search(&srv->name2clnt, name);

    if (group == NULL) {
        group = malloc(sizeof(dime_group_t));
        if (group == NULL) {
            return -1;
        }

        group->name = strdup(name);
        if (group->name == NULL) {
            free(group);

            return -1;
        }

        group->clnts_len = 0;
        group->clnts_cap = 4;

        group->clnts = malloc(sizeof(dime_client_t *) * group->clnts_cap);
        if (group->clnts == NULL) {
            free(group->name);
            free(group);

            return -1;
        }

        if (dime_table_insert(&srv->name2clnt, group->name, group) < 0) {
            free(group->clnts);
            free(group->name);
            free(group);

            return -1;
        }
    }

    if (clnt->groups_len >= clnt->groups_cap) {
        size_t ncap = (clnt->groups_cap * 3) / 2;

        dime_group_t **ngroups = realloc(clnt->groups, sizeof(dime_group_t *) * ncap);
        if (ngroups == NULL) {
            return -1;
        }

        clnt->groups = ngroups;
        clnt->groups_cap = ncap;
    }

    if (group->clnts_len >= group->clnts_cap) {
        size_t ncap = (group->clnts_cap * 3) / 2;

        dime_client_t **nclnts = realloc(group->clnts, sizeof(dime_client_t *) * ncap);
        if (nclnts == NULL) {
            return -1;
        }

        group->clnts = nclnts;
        group->clnts_cap = ncap;
    }

    clnt->groups[clnt->groups_len] = group;
    clnt->groups_len++;

    group->clnts[group->clnts_len] = clnt;
    group->clnts_len++;

    return 0;
}

int dime_client_leave(dime_client_t *clnt, dime_server_t *srv, json_t *jsondata, void *bindata, size_t bindata_len) {
    const char *name;

    if (json_unpack(jsondata, "{ss}", "name", &name) < 0) {
        return -1;
    }

    for (size_t i = 0; i < clnt->groups_len; i++) {
        dime_group_t *group = clnt->groups[i];

        if (strcmp(name, group->name) == 0) {
            for (size_t j = 0; j < group->clnts_len; j++) {
                if (group->clnts[j] == clnt) {
                    group->clnts_len--;
                    group->clnts[j] = group->clnts[group->clnts_len];

                    return 0;
                }
            }

            return -1;
        }
    }

    return -1;
}

int dime_client_send(dime_client_t *clnt, dime_server_t *srv, json_t *jsondata, void *bindata, size_t bindata_len) {
    const char *name;

    if (json_unpack(jsondata, "{ss}", "name", &name) < 0) {
        return -1;
    }

    dime_group_t *group = dime_table_search(&srv->name2clnt, name);
    if (group == NULL) {
        return -1;
    }

    if (group->clnts_len == 0) {
        return -1;
    }

    dime_rcmessage_t *msg = malloc(sizeof(dime_rcmessage_t) + bindata_len);
    if (msg == NULL) {
        return -1;
    }

    msg->refs = 0;
    msg->jsondata = jsondata;
    msg->bindata_len = bindata_len;

    memcpy(msg->bindata, bindata, bindata_len);
    json_incref(jsondata);

    for (size_t i = 0; i < group->clnts_len; i++) {
        if (dime_deque_pushr(&group->clnts[i]->queue, msg) < 0) {
            if (msg->refs == 0) {
                json_decref(jsondata);
                free(msg);
            }

            return -1;
        }

        msg->refs++;
    }

    assert(msg->refs > 0);

    return 0;
}

int dime_client_broadcast(dime_client_t *clnt, dime_server_t *srv, json_t *jsondata, void *bindata, size_t bindata_len) {
    dime_rcmessage_t *msg = malloc(sizeof(dime_rcmessage_t) + bindata_len);
    if (msg == NULL) {
        return -1;
    }

    msg->refs = 0;
    msg->jsondata = jsondata;
    msg->bindata_len = bindata_len;

    memcpy(msg->bindata, bindata, bindata_len);
    json_incref(jsondata);

    dime_table_iter_t it;

    dime_table_iter_init(&it, &srv->fd2clnt);

    while (dime_table_iter_next(&it)) {
        dime_client_t *other = it.val;

        if (clnt->fd != other->fd) {
            if (dime_deque_pushr(&other->queue, msg) < 0) {
                if (msg->refs == 0) {
                    json_decref(jsondata);
                    free(msg);
                }

                return -1;
            }

            msg->refs++;
        }
    }

    if (msg->refs == 0) {
        json_decref(jsondata);
        free(msg);
    }

    return 0;
}

int dime_client_sync(dime_client_t *clnt, dime_server_t *srv, json_t *jsondata, void *bindata, size_t bindata_len) {
    json_int_t n;

    if (json_unpack(jsondata, "{sI}", "n", &n) < 0) {
        return -1;
    }

    if (n < 0) {
        while (1) {
            dime_rcmessage_t *msg = dime_deque_popl(&clnt->queue);

            if (msg == NULL) {
                break;
            }

            if (dime_socket_push(&clnt->sock, msg->jsondata, msg->bindata, msg->bindata_len) < 0) {
                dime_deque_pushl(&clnt->queue, msg);

                return -1;
            }

            msg->refs--;

            if (msg->refs == 0) {
                json_decref(msg->jsondata);
                free(msg);
            }
        }
    } else {
        for (size_t i = 0; i < n; i++) {
            dime_rcmessage_t *msg = dime_deque_popl(&clnt->queue);

            if (msg == NULL) {
                break;
            }

            if (dime_socket_push(&clnt->sock, msg->jsondata, msg->bindata, msg->bindata_len) < 0) {
                dime_deque_pushl(&clnt->queue, msg);

                return -1;
            }

            msg->refs--;

            if (msg->refs == 0) {
                json_decref(msg->jsondata);
                free(msg);
            }
        }
    }

    if (dime_socket_push_str(&clnt->sock, "{}", NULL, 0) < 0) {
        return -1;
    }

    return 0;
}

int dime_client_devices(dime_client_t *clnt, dime_server_t *srv, json_t *jsondata, void *bindata, size_t bindata_len) {
    json_t *arr = json_array();
    if (arr == NULL) {
    }

    dime_table_iter_t it;

    dime_table_iter_init(&it, &srv->name2clnt);

    while (dime_table_iter_next(&it)) {
        dime_group_t *group = it.val;

        if (group->clnts_len > 0) {
            json_t *str = json_string(group->name);
            if (str == NULL) {
                json_decref(arr);

                return -1;
            }

            if (json_array_append_new(arr, str) < 0) {
                json_decref(str);
                json_decref(arr);

                return -1;
            }
        }
    }

    json_t *response = json_pack("{so}", "devices", jsondata);
    if (response == NULL) {
        json_decref(arr);

        return -1;
    }

    if (dime_socket_push(&clnt->sock, response, NULL, 0) < 0) {
        json_decref(response);

        return -1;
    }

    json_decref(response);

    return 0;
}
