#include <dbic++.h>
#include <ruby/ruby.h>
#include <ruby/io.h>
#include <time.h>
#include <unistd.h>

#define CONST_GET(scope, constant) rb_const_get(scope, rb_intern(constant))

static VALUE mSwift;
static VALUE cAdapter;
static VALUE cStatement;
static VALUE cResultSet;
static VALUE cPool;
static VALUE cRequest;
static VALUE cBigDecimal;

static VALUE eRuntimeError;
static VALUE eArgumentError;
static VALUE eStandardError;
static VALUE eConnectionError;

static VALUE fLoad;
static VALUE fStringify;
static VALUE fNew;

char errstr[8192];
static time_t tzoffset;

#define CSTRING(v) RSTRING_PTR(TYPE(v) == T_STRING ? v : rb_funcall(v, fStringify, 0))
#define TO_STRING(v) (TYPE(v) == T_STRING ? v : rb_funcall(v, fStringify, 0))

#define EXCEPTION(type) (dbi::ConnectionError &e) { \
    snprintf(errstr, 4096, "%s", e.what()); \
    rb_raise(eConnectionError, "%s : %s", type, errstr); \
} \
catch (dbi::Error &e) {\
    snprintf(errstr, 4096, "%s", e.what()); \
    rb_raise(eRuntimeError, "%s : %s", type, errstr); \
}


class IOStream : public dbi::IOStream {
    private:
    string empty;
    string data;
    VALUE callback;
    public:
    IOStream(VALUE cb) {
        callback = cb;
    }
    string& read() {
        VALUE stream = rb_proc_call(callback, rb_ary_new());
        if (stream == Qnil)
            return empty;
        else {
            if (TYPE(stream) != T_STRING)
                rb_raise(eArgumentError,
                    "Adapter#write can only process string data. You need to stringify values returned in the callback.");
            data = string(RSTRING_PTR(stream), RSTRING_LEN(stream));
            return data;
        }
    }

    // never return more data in the callback than a packet size which is 8k for mysql client by default.
    uint read(char *buffer, uint len) {
        VALUE stream = rb_proc_call(callback, rb_ary_new());
        if (stream == Qnil)
            return 0;
        else {
            len = len < RSTRING_LEN(stream) ? len : RSTRING_LEN(stream);
            memcpy(buffer, RSTRING_PTR(stream), len);
            return len;
        }
    }

    void write(const char *str) {
        rb_proc_call(callback, rb_ary_new3(1, rb_str_new2(str)));
    }
    void write(const char *str, unsigned long l) {
        rb_proc_call(callback, rb_ary_new3(1, rb_str_new(str, l)));
    }
    void truncate() {
        data = "";
    }
};


static dbi::Handle* DBI_HANDLE(VALUE self) {
    dbi::Handle *h;
    Data_Get_Struct(self, dbi::Handle, h);
    if (!h) rb_raise(eRuntimeError, "Invalid object, did you forget to call #super ?");
    return h;
}

static dbi::AbstractStatement* DBI_STATEMENT(VALUE self) {
    dbi::AbstractStatement *st;
    Data_Get_Struct(self, dbi::AbstractStatement, st);
    if (!st) rb_raise(eRuntimeError, "Invalid object, did you forget to call #super ?");
    return st;
}

static dbi::ConnectionPool* DBI_CPOOL(VALUE self) {
    dbi::ConnectionPool *cp;
    Data_Get_Struct(self, dbi::ConnectionPool, cp);
    if (!cp) rb_raise(eRuntimeError, "Invalid object, did you forget to call #super ?");
    return cp;
}

static dbi::Request* DBI_REQUEST(VALUE self) {
    dbi::Request *r;
    Data_Get_Struct(self, dbi::Request, r);
    if (!r) rb_raise(eRuntimeError, "Invalid object, did you forget to call #super ?");
    return r;
}

void static inline rb_extract_bind_params(int argc, VALUE* argv, std::vector<dbi::Param> &bind) {
    for (int i = 0; i < argc; i++) {
        VALUE arg = argv[i];
        if (arg == Qnil)
            bind.push_back(dbi::PARAM(dbi::null()));
        else {
            arg = TO_STRING(arg);
            bind.push_back(dbi::PARAM_BINARY((unsigned char*)RSTRING_PTR(arg), RSTRING_LEN(arg)));
        }
    }
}

VALUE rb_swift_init(VALUE self, VALUE path) {
    try { dbi::dbiInitialize(CSTRING(path)); } catch EXCEPTION("Swift#init");
    return Qtrue;
}

static void free_connection(dbi::Handle *self) {
    if (self) delete self;
}

VALUE rb_adapter_alloc(VALUE klass) {
    dbi::Handle *h = 0;
    return Data_Wrap_Struct(klass, NULL, free_connection, h);
}

VALUE rb_adapter_init(VALUE self, VALUE opts) {
    VALUE db       = rb_hash_aref(opts, ID2SYM(rb_intern("db")));
    VALUE host     = rb_hash_aref(opts, ID2SYM(rb_intern("host")));
    VALUE port     = rb_hash_aref(opts, ID2SYM(rb_intern("port")));
    VALUE user     = rb_hash_aref(opts, ID2SYM(rb_intern("user")));
    VALUE driver   = rb_hash_aref(opts, ID2SYM(rb_intern("driver")));
    VALUE password = rb_hash_aref(opts, ID2SYM(rb_intern("password")));

    if (NIL_P(db)) rb_raise(eArgumentError, "Adapter#new called without :db");
    if (NIL_P(driver)) rb_raise(eArgumentError, "Adapter#new called without :driver");

    host     = NIL_P(host)     ? rb_str_new2("") : host;
    port     = NIL_P(port)     ? rb_str_new2("") : port;
    user     = NIL_P(user)     ? rb_str_new2(getlogin()) : user;
    password = NIL_P(password) ? rb_str_new2("") : password;

    try {
        DATA_PTR(self) = new dbi::Handle(
            CSTRING(driver), CSTRING(user), CSTRING(password),
            CSTRING(db), CSTRING(host), CSTRING(port)
        );
    } catch EXCEPTION("Adapter#new");

    rb_iv_set(self, "@options", opts);
    return Qnil;
}

VALUE rb_adapter_close(VALUE self) {
    dbi::Handle *h = DBI_HANDLE(self);
    try {
        h->close();
    } catch EXCEPTION("Adapter#close");
    return Qtrue;
}

static void free_statement(dbi::AbstractStatement *self) {
    if (self) {
        self->cleanup();
        delete self;
    }
}

static VALUE rb_adapter_prepare(int argc, VALUE *argv, VALUE self) {
    VALUE sql, scheme, prepared;
    dbi::Handle *h = DBI_HANDLE(self);

    rb_scan_args(argc, argv, "11", &scheme, &sql);
    if (TYPE(scheme) != T_CLASS) {
        sql    = scheme;
        scheme = Qnil;
    }

    try {
        dbi::AbstractStatement *st = h->conn()->prepare(CSTRING(sql));
        prepared = Data_Wrap_Struct(cStatement, NULL, free_statement, st);
        rb_iv_set(prepared, "@scheme", scheme);
    } catch EXCEPTION("Adapter#prepare");

    return prepared;
}

VALUE rb_adapter_execute(int argc, VALUE *argv, VALUE self) {
    unsigned int rows = 0;
    dbi::Handle *h = DBI_HANDLE(self);
    if (argc == 0 || NIL_P(argv[0]))
        rb_raise(eArgumentError, "Adapter#execute called without a SQL command");
    try {
        if (argc == 1) {
            rows = h->execute(CSTRING(argv[0]));
        }
        else {
            dbi::ResultRow bind;
            rb_extract_bind_params(argc-1, argv+1, bind);
            dbi::AbstractStatement *st = h->conn()->prepare(CSTRING(argv[0]));
            if (dbi::_trace)
                dbi::logMessage(dbi::_trace_fd, dbi::formatParams(CSTRING(argv[0]), bind));
            rows = st->execute(bind);
            delete st;
        }
    } catch EXCEPTION("Adapter#execute");
    return INT2NUM(rows);
}

VALUE rb_adapter_begin(int argc, VALUE *argv, VALUE self) {
    dbi::Handle *h = DBI_HANDLE(self);
    VALUE save;
    rb_scan_args(argc, argv, "01", &save);
    try { NIL_P(save) ? h->begin() : h->begin(CSTRING(save)); } catch EXCEPTION("Adapter#begin");
}

VALUE rb_adapter_commit(int argc, VALUE *argv, VALUE self) {
    dbi::Handle *h = DBI_HANDLE(self);
    VALUE save;
    rb_scan_args(argc, argv, "01", &save);
    try { NIL_P(save) ? h->commit() : h->commit(CSTRING(save)); } catch EXCEPTION("Adapter#commit");
}

VALUE rb_adapter_rollback(int argc, VALUE *argv, VALUE self) {
    dbi::Handle *h = DBI_HANDLE(self);
    VALUE save_point;
    rb_scan_args(argc, argv, "01", &save_point);
    try { NIL_P(save_point) ? h->rollback() : h->rollback(CSTRING(save_point)); } catch EXCEPTION("Adapter#rollback");
}

VALUE rb_adapter_transaction(int argc, VALUE *argv, VALUE self) {
    int status;
    VALUE sp, block;
    rb_scan_args(argc, argv, "01&", &sp, &block);

    std::string save_point = NIL_P(sp) ? "SP" + dbi::generateCompactUUID() : CSTRING(sp);
    dbi::Handle *h         = DBI_HANDLE(self);

    try {
        h->begin(save_point);
        rb_protect(rb_yield, self, &status);
        if (status == 0 && h->transactions().back() == save_point) {
            h->commit(save_point);
        }
        else if (status != 0) {
            if (h->transactions().back() == save_point) h->rollback(save_point);
            rb_jump_tag(status);
        }
    } catch EXCEPTION("Adapter#transaction{}");
}

VALUE rb_adapter_write(int argc, VALUE *argv, VALUE self) {
    VALUE callback, table, fields;
    unsigned long rows = 0;
    rb_scan_args(argc, argv, "1*&", &table, &fields, &callback);
    if (NIL_P(callback))
        rb_raise(eArgumentError, "Adapter#write called without a block");

    dbi::Handle *h = DBI_HANDLE(self);
    IOStream io(callback);
    try {
        dbi::ResultRow rfields;
        for (int n = 0; n < RARRAY_LEN(fields); n++) {
            VALUE f = rb_ary_entry(fields, n);
            rfields << std::string(RSTRING_PTR(f), RSTRING_LEN(f));
        }
        // This is just for the friggin mysql support - mysql does not like a statement close
        // command being send on a handle when the writing has started.
        rb_gc();
        rows = h->copyIn(RSTRING_PTR(table), rfields, &io);
    } catch EXCEPTION("Adapter#write");

    return ULONG2NUM(rows);
}

VALUE rb_statement_alloc(VALUE klass) {
    dbi::AbstractStatement *st = 0;
    return Data_Wrap_Struct(klass, NULL, free_statement, st);
}

VALUE rb_statement_init(VALUE self, VALUE hl, VALUE sql) {
    dbi::Handle *h = DBI_HANDLE(hl);

    if (NIL_P(hl) || !h)
        rb_raise(eArgumentError, "Statement#new called without an Adapter instance");
    if (NIL_P(sql))
        rb_raise(eArgumentError, "Statement#new called without a SQL command");

    try {
        DATA_PTR(self) = h->conn()->prepare(CSTRING(sql));
    } catch EXCEPTION("Statement#new");

    return Qnil;
}

static VALUE rb_statement_each(VALUE self);

VALUE rb_statement_execute(int argc, VALUE *argv, VALUE self) {
    dbi::AbstractStatement *st = DBI_STATEMENT(self);
    try {
        if (argc == 0) {
            dbi::ResultRow params;
            if (dbi::_trace)
                dbi::logMessage(dbi::_trace_fd, dbi::formatParams(st->command(), params));
            st->execute();
        }
        else {
            dbi::ResultRow bind;
            rb_extract_bind_params(argc, argv, bind);
            if (dbi::_trace)
                dbi::logMessage(dbi::_trace_fd, dbi::formatParams(st->command(), bind));
            st->execute(bind);
        }
    } catch EXCEPTION("Statement#execute");

    if (rb_block_given_p()) return rb_statement_each(self);
    return self;
}

VALUE rb_statement_finish(VALUE self) {
    dbi::AbstractStatement *st = DBI_STATEMENT(self);
    try {
        st->finish();
    } catch EXCEPTION("Statement#finish");
}

VALUE rb_statement_rows(VALUE self) {
    unsigned int rows;
    dbi::AbstractStatement *st = DBI_STATEMENT(self);
    try { rows = st->rows(); } catch EXCEPTION("Statement#rows");
    return INT2NUM(rows);
}

VALUE rb_statement_insert_id(VALUE self) {
  dbi::AbstractStatement *st = DBI_STATEMENT(self);
  VALUE insert_id    = Qnil;
  try {
    if (st->rows() > 0) insert_id = LONG2NUM(st->lastInsertID());
  } catch EXCEPTION("Statement#insert_id");

  return insert_id;
}

VALUE rb_field_typecast(int type, const char *data, unsigned long len) {
    time_t epoch, offset;
    struct tm tm;

    char datetime[512], tzsign = '+';
    int hour = 0, min = 0, sec = 0, tzhour = 0, tzmin = 0;
    double usec = 0;

    switch(type) {
        case DBI_TYPE_BOOLEAN:
            return strcmp(data, "t") == 0 || strcmp(data, "1") == 0 ? Qtrue : Qfalse;
        case DBI_TYPE_INT:
            return rb_cstr2inum(data, 10);
        case DBI_TYPE_TEXT:
            return rb_str_new(data, len);
        case DBI_TYPE_TIME:
            // if timestamp field has usec resolution, parse it.
            if (strlen(data) > 19 && data[19] == '.') {
                sscanf(data, "%s %d:%d:%d%lf%c%02d%02d",
                    datetime, &hour, &min, &sec, &usec, &tzsign, &tzhour, &tzmin);
            }
            else {
                sscanf(data, "%s %d:%d:%d%c%02d%02d",
                    datetime, &hour, &min, &sec, &tzsign, &tzhour, &tzmin);
            }
            sprintf(datetime, "%s %02d:%02d:%02d", datetime, hour, min, sec);
            memset(&tm, 0, sizeof(struct tm));
            if (strptime(datetime, "%F %T", &tm)) {
                offset = 0;
                epoch  = mktime(&tm);
                if (tzhour > 0 || tzmin > 0) {
                    offset = tzsign == '+' ?
                          (time_t)tzhour * -3600 + (time_t)tzmin * -60
                        : (time_t)tzhour *  3600 + (time_t)tzmin *  60;
                    offset += tzoffset;
                }
                return rb_time_new(epoch + offset, usec*1000000);
            }
            else {
                fprintf(stderr, "typecast failed to parse date: %s\n", data);
                return rb_str_new(data, len);
            }
        // does bigdecimal solve all floating point woes ? dunno :)
        case DBI_TYPE_NUMERIC:
            return rb_funcall(cBigDecimal, fNew, 1, rb_str_new2(data));
        case DBI_TYPE_FLOAT:
            return rb_float_new(atof(data));
    }
}

static VALUE rb_statement_each(VALUE self) {
    unsigned int r, c;
    unsigned long len;
    const char *data;

    dbi::AbstractStatement *st = DBI_STATEMENT(self);
    VALUE scheme = rb_iv_get(self, "@scheme");

    try {
        VALUE attrs = rb_ary_new();
        std::vector<string> fields = st->fields();
        std::vector<int> types     = st->types();
        for (c = 0; c < fields.size(); c++) {
            rb_ary_push(attrs, ID2SYM(rb_intern(fields[c].c_str())));
        }

        // TODO Code duplication
        //      Avoiding a rb_yield(NIL_P(scheme) ? row : rb_funcall(scheme, load, row))
        //      Maybe an inline method will help ?
        st->seek(0);
        if (NIL_P(scheme) || scheme == Qnil) {
            for (r = 0; r < st->rows(); r++) {
                VALUE row = rb_hash_new();
                for (c = 0; c < st->columns(); c++) {
                    data = (const char*)st->fetchValue(r,c, &len);
                    if (data)
                        rb_hash_aset(row, rb_ary_entry(attrs, c), rb_field_typecast(types[c], data, len));
                    else
                        rb_hash_aset(row, rb_ary_entry(attrs, c), Qnil);
                }
                rb_yield(row);
            }
        }
        else {
            for (r = 0; r < st->rows(); r++) {
                VALUE row = rb_hash_new();
                for (c = 0; c < st->columns(); c++) {
                    data = (const char*)st->fetchValue(r,c, &len);
                    if (data)
                        rb_hash_aset(row, rb_ary_entry(attrs, c), rb_field_typecast(types[c], data, len));
                    else
                        rb_hash_aset(row, rb_ary_entry(attrs, c), Qnil);
                }
                rb_yield(rb_funcall(scheme, fLoad, 1, row));
            }
        }
    } catch EXCEPTION("Statment#each");
    return Qnil;
}

VALUE rb_statement_fetchrow(VALUE self) {
    const char *data;
    unsigned int r, c;
    unsigned long len;
    VALUE row = Qnil;
    dbi::AbstractStatement *st = DBI_STATEMENT(self);
    try {
        r = st->currentRow();
        if (r < st->rows()) {
            row = rb_ary_new();
            for (c = 0; c < st->columns(); c++) {
                data = (const char*)st->fetchValue(r, c, &len);
                rb_ary_push(row, data ? rb_str_new(data, len) : Qnil);
            }
        }
    } catch EXCEPTION("Statement#fetchrow");

    return row;
}

VALUE rb_statement_rewind(VALUE self) {
    dbi::AbstractStatement *st = DBI_STATEMENT(self);
    try { st->rewind(); } catch EXCEPTION("Statement#rewind");
    return Qnil;
}

VALUE rb_swift_trace(int argc, VALUE *argv, VALUE self) {
    // by default log all messages to stderr.
    int fd = 2;
    rb_io_t *fptr;
    VALUE flag, io;

    rb_scan_args(argc, argv, "11", &flag, &io);

    if (TYPE(flag) != T_TRUE && TYPE(flag) != T_FALSE)
        rb_raise(eArgumentError, "Swift#trace expects a boolean flag, got %s", CSTRING(flag));

    if (!NIL_P(io)) {
        GetOpenFile(rb_convert_type(io, T_FILE, "IO", "to_io"), fptr);
        fd = fptr->fd;
    }

    dbi::trace(flag == Qtrue ? true : false, fd);
}

VALUE rb_adapter_dup(VALUE self) {
    rb_raise(eRuntimeError, "Adapter#dup or Adapter#clone is not allowed.");
}

VALUE rb_statement_dup(VALUE self) {
    rb_raise(eRuntimeError, "Statement#dup or Statement#clone is not allowed.");
}

static void free_request(dbi::Request *self) {
    if(self) delete self;
}

VALUE rb_request_alloc(VALUE klass) {
    dbi::Request *r = 0;
    return Data_Wrap_Struct(klass, NULL, free_request, r);
}

static void free_cpool(dbi::ConnectionPool *self) {
    if (self) delete self;
}

VALUE rb_cpool_alloc(VALUE klass) {
    dbi::ConnectionPool *c = 0;
    return Data_Wrap_Struct(klass, NULL, free_cpool, c);
}

VALUE rb_cpool_init(VALUE self, VALUE n, VALUE opts) {
    VALUE db       = rb_hash_aref(opts, ID2SYM(rb_intern("db")));
    VALUE host     = rb_hash_aref(opts, ID2SYM(rb_intern("host")));
    VALUE port     = rb_hash_aref(opts, ID2SYM(rb_intern("port")));
    VALUE user     = rb_hash_aref(opts, ID2SYM(rb_intern("user")));
    VALUE driver   = rb_hash_aref(opts, ID2SYM(rb_intern("driver")));
    VALUE password = rb_hash_aref(opts, ID2SYM(rb_intern("password")));

    if (NIL_P(db)) rb_raise(eArgumentError, "ConnectionPool#new called without :db");
    if (NIL_P(driver)) rb_raise(eArgumentError, "ConnectionPool#new called without :driver");

    host     = NIL_P(host)     ? rb_str_new2("") : host;
    port     = NIL_P(port)     ? rb_str_new2("") : port;
    user     = NIL_P(user)     ? rb_str_new2(getlogin()) : user;
    password = NIL_P(password) ? rb_str_new2("") : password;

    if (NUM2INT(n) < 1) rb_raise(eArgumentError, "ConnectionPool#new called with invalid pool size.");

    try {
        DATA_PTR(self) = new dbi::ConnectionPool(
            NUM2INT(n), CSTRING(driver), CSTRING(user), CSTRING(password), CSTRING(db), CSTRING(host), CSTRING(port)
        );
    } catch EXCEPTION("ConnectionPool#new");

    return Qnil;
}

void rb_cpool_callback(dbi::AbstractResultSet *rs) {
    VALUE callback = (VALUE)rs->context;
    if (!NIL_P(callback))
        rb_proc_call(callback, rb_ary_new3(1, Data_Wrap_Struct(cResultSet, 0, 0, rs)));
}

VALUE rb_cpool_execute(int argc, VALUE *argv, VALUE self) {
    dbi::ConnectionPool *cp = DBI_CPOOL(self);
    int n;
    VALUE sql;
    VALUE args;
    VALUE callback;
    VALUE request = Qnil;

    rb_scan_args(argc, argv, "1*&", &sql, &args, &callback);
    try {
        dbi::ResultRow bind;
        for (n = 0; n < RARRAY_LEN(args); n++) {
            VALUE arg = rb_ary_entry(args, n);
            if (arg == Qnil)
                bind.push_back(dbi::PARAM(dbi::null()));
            else {
                arg = TO_STRING(arg);
                bind.push_back(dbi::PARAM_BINARY((unsigned char*)RSTRING_PTR(arg), RSTRING_LEN(arg)));
            }
        }
        // TODO GC mark callback.
        request = rb_request_alloc(cRequest);
        DATA_PTR(request) = cp->execute(CSTRING(sql), bind, rb_cpool_callback, (void*)callback);
    } catch EXCEPTION("ConnectionPool#execute");

    return DATA_PTR(request) ? request : Qnil;
}

VALUE rb_request_socket(VALUE self) {
    dbi::Request *r = DBI_REQUEST(self);
    VALUE fd = Qnil;
    try {
        fd = INT2NUM(r->socket());
    } catch EXCEPTION("Request#socket");
    return fd;
}

VALUE rb_request_process(VALUE self) {
    VALUE rc = Qnil;
    dbi::Request *r = DBI_REQUEST(self);

    try {
        rc = r->process() ? Qtrue : Qfalse;
    } catch EXCEPTION("Request#process");

    return rc;
}

extern "C" {
    void Init_swift(void) {
        rb_require("bigdecimal");

        fNew             = rb_intern("new");
        fStringify       = rb_intern("to_s");
        fLoad            = rb_intern("load");

        eRuntimeError    = CONST_GET(rb_mKernel, "RuntimeError");
        eArgumentError   = CONST_GET(rb_mKernel, "ArgumentError");
        eStandardError   = CONST_GET(rb_mKernel, "StandardError");
        cBigDecimal      = CONST_GET(rb_mKernel, "BigDecimal");
        eConnectionError = rb_define_class("ConnectionError", eRuntimeError);

        mSwift           = rb_define_module("Swift");
        cAdapter         = rb_define_class_under(mSwift, "Adapter", rb_cObject);
        cStatement       = rb_define_class_under(mSwift, "Statement", rb_cObject);
        cResultSet       = rb_define_class_under(mSwift, "ResultSet", cStatement);
        cPool            = rb_define_class_under(mSwift, "ConnectionPool", rb_cObject);
        cRequest         = rb_define_class_under(mSwift, "Request", rb_cObject);

        rb_define_module_function(mSwift, "init",  RUBY_METHOD_FUNC(rb_swift_init), 1);
        rb_define_module_function(mSwift, "trace", RUBY_METHOD_FUNC(rb_swift_trace), -1);

        rb_define_alloc_func(cAdapter, rb_adapter_alloc);

        rb_define_method(cAdapter, "initialize",  RUBY_METHOD_FUNC(rb_adapter_init), 1);
        rb_define_method(cAdapter, "prepare",     RUBY_METHOD_FUNC(rb_adapter_prepare), -1);
        rb_define_method(cAdapter, "execute",     RUBY_METHOD_FUNC(rb_adapter_execute), -1);
        rb_define_method(cAdapter, "begin",       RUBY_METHOD_FUNC(rb_adapter_begin), -1);
        rb_define_method(cAdapter, "commit",      RUBY_METHOD_FUNC(rb_adapter_commit), -1);
        rb_define_method(cAdapter, "rollback",    RUBY_METHOD_FUNC(rb_adapter_rollback), -1);
        rb_define_method(cAdapter, "transaction", RUBY_METHOD_FUNC(rb_adapter_transaction), -1);
        rb_define_method(cAdapter, "close",       RUBY_METHOD_FUNC(rb_adapter_close), 0);
        rb_define_method(cAdapter, "dup",         RUBY_METHOD_FUNC(rb_adapter_dup), 0);
        rb_define_method(cAdapter, "clone",       RUBY_METHOD_FUNC(rb_adapter_dup), 0);
        rb_define_method(cAdapter, "write",       RUBY_METHOD_FUNC(rb_adapter_write), -1);

        rb_define_alloc_func(cStatement, rb_statement_alloc);

        rb_define_method(cStatement, "initialize",  RUBY_METHOD_FUNC(rb_statement_init), 2);
        rb_define_method(cStatement, "execute",     RUBY_METHOD_FUNC(rb_statement_execute), -1);
        rb_define_method(cStatement, "each",        RUBY_METHOD_FUNC(rb_statement_each), 0);
        rb_define_method(cStatement, "rows",        RUBY_METHOD_FUNC(rb_statement_rows), 0);
        rb_define_method(cStatement, "fetchrow",    RUBY_METHOD_FUNC(rb_statement_fetchrow), 0);
        rb_define_method(cStatement, "finish",      RUBY_METHOD_FUNC(rb_statement_finish), 0);
        rb_define_method(cStatement, "dup",         RUBY_METHOD_FUNC(rb_statement_dup), 0);
        rb_define_method(cStatement, "clone",       RUBY_METHOD_FUNC(rb_statement_dup), 0);
        rb_define_method(cStatement, "insert_id",   RUBY_METHOD_FUNC(rb_statement_insert_id), 0);
        rb_define_method(cStatement, "rewind",      RUBY_METHOD_FUNC(rb_statement_rewind), 0);

        rb_include_module(cStatement, CONST_GET(rb_mKernel, "Enumerable"));


        rb_define_alloc_func(cPool, rb_cpool_alloc);

        rb_define_method(cPool, "initialize",  RUBY_METHOD_FUNC(rb_cpool_init), 2);
        rb_define_method(cPool, "execute",     RUBY_METHOD_FUNC(rb_cpool_execute), -1);

        rb_define_alloc_func(cRequest, rb_request_alloc);

        rb_define_method(cRequest, "socket",      RUBY_METHOD_FUNC(rb_request_socket), 0);
        rb_define_method(cRequest, "process",     RUBY_METHOD_FUNC(rb_request_process), 0);

        rb_define_method(cResultSet, "execute", RUBY_METHOD_FUNC(Qnil), 0);

        tzset();
        tzoffset = -1 * timezone;
        // avoids all those stat("/etc/localtime") calls.
        if (!getenv("TZ")) {
            char buffer[128];
            int hour = timezone/3600;
            int min  = abs(timezone) - 3600*abs(hour);
            snprintf(buffer, 127, "%s%+02d:%02d", tzname[0], hour, min);
            setenv("TZ", buffer, 0);
        }
    }
}