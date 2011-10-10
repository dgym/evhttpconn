#include "evhttpconn.h"

#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <stdio.h>
#include <ctype.h>

typedef evhttp_buffer_t buffer_t;
typedef struct ev_loop ev_loop_t;
typedef struct ev_io ev_io_t;
typedef evhttp_connection_t connection_t;

///
// Buffers
///

static void buffer_init(buffer_t *self)
{
    self->data = NULL;
    self->start = 0;
    self->size = 0;
    self->allocated = 0;
}

static void buffer_free(buffer_t *self)
{
    free(self->data);
    buffer_init(self);
}

static int buffer_allocate(buffer_t *self, int size)
{
    if (size == self->allocated)
        return 0;

    char *data = (char *)realloc(self->data, size);
    if (!data)
        return -1;

    self->data = data;
    self->allocated = size;
    return 0;
}

static int buffer_make_space(buffer_t *self, int size)
{
    int new_size;

    new_size = self->allocated;
    if (new_size < 4096)
        new_size = 4096;

    while (new_size - self->size < size)
        new_size <<= 1;

    return buffer_allocate(self, new_size);
}

static int buffer_find_chr(buffer_t *self, char c)
{
    int i;
    char *data = (char *)self->data;
    for (i=self->start; i<self->size; ++i)
    {
        if (data[i] == c)
            return i;
    }
    return -1;
}


///
// Connections
///

#define CLOSE_OK 0
#define CLOSE_DELAY 1
#define CLOSE_REQESTED 2

static void on_read(ev_loop_t *loop, ev_io_t *watcher, int revents);
static void on_write(ev_loop_t *loop, ev_io_t *watcher, int revents);

void evhttp_connection_init(evhttp_connection_t *self,
                            struct ev_loop *loop,
                            int fd,
                            evhttp_connection_on_first_line on_first_line,
                            evhttp_connection_on_header on_header,
                            evhttp_connection_on_headers_end on_headers_end,
                            evhttp_connection_on_content on_chunk,
                            evhttp_connection_on_content on_complete_content,
                            evhttp_connection_on_complete on_complete,
                            evhttp_connection_on_close on_close,
                            void *callback_data)
{
    self->loop = loop;
    self->fd = fd;

    buffer_init(&self->read_buffer);
    buffer_init(&self->write_buffer);

    self->read_watcher.data = self;
    ev_io_init(&self->read_watcher, on_read, fd, EV_READ);

    self->write_watcher.data = self;
    ev_io_init(&self->write_watcher, on_write, fd, EV_WRITE);

    self->state = 0;
    self->content_length = -2;
    self->terminating = 0;
    self->closing = CLOSE_OK;

    self->on_first_line = on_first_line;
    self->on_header = on_header;
    self->on_headers_end = on_headers_end;
    self->on_chunk = on_chunk;
    self->on_complete_content = on_complete_content;
    self->on_complete = on_complete;
    self->on_close = on_close;
    self->callback_data = callback_data;
    
    ev_io_start(loop, &self->read_watcher);
}

void evhttp_connection_close(evhttp_connection_t *self)
{
    if (self->closing != CLOSE_OK)
    {
        self->closing = CLOSE_REQESTED;
        return;
    }

    ev_io_stop(self->loop, &self->read_watcher);
    ev_io_stop(self->loop, &self->write_watcher);

    buffer_free(&self->read_buffer);
    buffer_free(&self->write_buffer);

    if (self->on_close)
    {
        evhttp_connection_on_close on_close = self->on_close;
        self->on_close = NULL;
        on_close(self->callback_data);
    }
}

int evhttp_connection_send(evhttp_connection_t *self, evhttp_string_t data)
{
    if (buffer_make_space(&self->write_buffer, data.length) != 0)
        return -1;

    memcpy(self->write_buffer.data + self->write_buffer.size, data.data, data.length);
    self->write_buffer.size += data.length;
    ev_io_start(self->loop, &self->write_watcher);
    return 0;
}

void evhttp_connection_terminate(evhttp_connection_t *self)
{
    self->terminating = 1;
    if (self->write_buffer.start == self->write_buffer.size)
        evhttp_connection_close(self);
}

void on_read(ev_loop_t *loop, ev_io_t *watcher, int revents)
{
    connection_t *self = (connection_t *)watcher->data;
    int got;

    self->closing = CLOSE_DELAY;

    if (buffer_make_space(&self->read_buffer, 4096) != 0)
    {
        goto close;
    }

    got = read(self->fd, self->read_buffer.data + self->read_buffer.size, 4096);
    if (got <= 0)
    {
        if (self->state == 4)
        {
            if (self->on_complete)
                self->on_complete(self->callback_data);
        }
        goto close;
    }
    self->read_buffer.size += got;

    if (self->state == 0)
    {
        int idx = buffer_find_chr(&self->read_buffer, ' ');
        if (idx >= 0)
        {
            self->tmp[0] = self->read_buffer.start;
            self->tmp[1] = idx - self->read_buffer.start;
            self->read_buffer.start = idx + 1;
            self->state = 1;

            // if the message starts with HTTP then it is a
            // reply, so handle unspecified content-length
            self->content_length = -1;
        }
    }

    if (self->state == 1)
    {
        int idx = buffer_find_chr(&self->read_buffer, ' ');
        if (idx >= 0)
        {
            self->tmp[2] = self->read_buffer.start;
            self->tmp[3] = idx - self->read_buffer.start;
            self->read_buffer.start = idx + 1;
            self->state = 2;
        }
    }

    if (self->state == 2)
    {
        int idx = buffer_find_chr(&self->read_buffer, '\n');
        if (idx >= 0)
        {
            if (self->on_first_line)
            {
                int end = idx;
                if (end > self->read_buffer.start && self->read_buffer.data[end-1] == '\r')
                    --end;

                evhttp_string_t first, second, third;

                first.data = self->read_buffer.data + self->tmp[0];
                first.length = self->tmp[1];
                second.data = self->read_buffer.data + self->tmp[2];
                second.length = self->tmp[3];
                third.data = self->read_buffer.data + self->read_buffer.start;
                third.length = end - self->read_buffer.start;

                self->on_first_line(first, second, third, self->callback_data);
                if (self->closing == CLOSE_REQESTED)
                    goto close;
            }

            self->read_buffer.start = idx + 1;
            self->tmp[0] = 0; // chunked sent counter
            self->state = 3;
        }
    }

    if (self->state == 3)
    {
        int newline;
        for (newline = buffer_find_chr(&self->read_buffer, '\n'); newline >= 0; newline = buffer_find_chr(&self->read_buffer, '\n'))
        {
            int start = self->read_buffer.start;
            int end = newline;
            char *data = self->read_buffer.data;
            if (end > start && data[end-1] == '\r')
                --end;

            if (end == start)
            {
                self->read_buffer.start = newline + 1;
                self->state = 4;

                if (self->on_headers_end)
                {
                    evhttp_string_t message;
                    message.data = data;
                    message.length = newline+1;
                    self->on_headers_end(message, self->callback_data);
                    if (self->closing == CLOSE_REQESTED)
                        goto close;
                }

                break;
            }

            int key_start, key_end, value_start, value_end;
            int idx;
            for (idx=start; idx<end; ++idx)
            {
                char c = data[idx];
                if (c != ' ' && c != '\t')
                    break;
            }
            key_start = idx;

            for (; idx<end; ++idx)
            {
                char c = data[idx];
                if (c == ':' || c == ' ' || c == '\t')
                    break;
                data[idx] = tolower(c);
            }
            key_end = idx;

            for (; idx<end; ++idx)
            {
                char c = data[idx];
                if (c == ':')
                    break;
            }

            for (++idx; idx<end; ++idx)
            {
                char c = data[idx];
                if (c != ' ' && c != '\t')
                    break;
            }
            value_start = idx;

            for (idx=end-1; idx>value_start; --idx)
            {
                char c = data[idx];
                if (c != ' ' && c != '\t')
                    break;
            }
            value_end = idx+1;

            evhttp_string_t key, value;
            key.data = data + key_start;
            key.length = key_end - key_start;
            value.data = data + value_start;
            value.length = value_end - value_start;

            if (self->content_length < 0 && key.length == 14 && !memcmp(data + key_start, "content-length", 14))
            {
                char *endptr;
                int l = strtol(value.data, &endptr, 10);
                if (l >= 0)
                    self->content_length = l;
                // TODO, check endptr
            }

            if (self->on_header)
            {
                self->on_header(key, value, self->callback_data);
                if (self->closing == CLOSE_REQESTED)
                    goto close;
            }

            self->read_buffer.start = newline + 1;
        }
    }

    if (self->state == 4)
    {
        if (self->content_length < -1)
            self->state = 5;
        else
        {
            int len = self->read_buffer.size - self->read_buffer.start;

            if (self->on_complete_content)
            {
                if (self->content_length >= 0 && self->content_length <= len)
                {
                    self->state = 5;
                    evhttp_string_t content;
                    content.data = self->read_buffer.data + self->read_buffer.start;
                    content.length = self->content_length;
                    self->on_complete_content(content, self->callback_data);
                    if (self->closing == CLOSE_REQESTED)
                        goto close;
                }
            }
            else
            {
                if (self->on_chunk)
                {
                    evhttp_string_t content;
                    content.data = self->read_buffer.data + self->read_buffer.start;
                    content.length = len;
                    self->on_chunk(content, self->callback_data);
                    if (self->closing == CLOSE_REQESTED)
                        goto close;
                }

                self->tmp[0] += len;
                if (self->content_length >= 0 && self->content_length <= self->tmp[0])
                    self->state = 5;
                else
                {
                    self->read_buffer.start = 0;
                    self->read_buffer.size = 0;
                }
            }
        }
    }

    if (self->state == 5)
    {
        // keep alive not currently supported
        // goto terminal state 6
        self->state = 6;

        if (self->on_complete)
        {
            self->on_complete(self->callback_data);
            if (self->closing == CLOSE_REQESTED)
                goto close;
        }

        ev_io_stop(self->loop, &self->read_watcher);
    }

    self->closing = CLOSE_OK;
    return;
close:
    self->closing = CLOSE_OK;
    evhttp_connection_close(self);
}

void on_write(ev_loop_t *loop, ev_io_t *watcher, int revents)
{
    connection_t *self = (connection_t *)watcher->data;
    int start = self->write_buffer.start;
    int sent = write(self->fd, self->write_buffer.data + start, self->write_buffer.size - start);
    if (sent < 0)
        goto close;
    self->write_buffer.start += sent;
    if (self->terminating && self->write_buffer.start == self->write_buffer.size)
        goto close;
    if (self->write_buffer.start == self->write_buffer.size)
    {
        self->write_buffer.start = 0;
        self->write_buffer.size = 0;
        ev_io_stop(self->loop, &self->write_watcher);
    }
    return;

close:
    evhttp_connection_close(self);
}
