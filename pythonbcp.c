//=================================================================================
//                              Python BCP
#define python_bcp_moduledoc "\
This python module allows python programs to utilise the bulk copying\n\
capabilities of Sybase or Microsoft SQL Server via the freetds library.\n\
(www.freetds.org)\n\n\
e.g.\n\n\
   bcp.use_interfaces('/etc/freetds/freetds.conf')\n\n\
   connection = bcp.Connection(server='server', username='me', password='****', database='mydb', batchsize=0)\n\n\
   connection.init('mytable')\n\n\
   for row in ROWS:\n\
        connection.send(row)\n\n\
   connection.done()\
   connection.disconnect()\
"

// --------------------------------------------------------------------------------
// March 24, 2017:   DKW Fix for FreeTDS + Anaconda + Python 2.7 on Windows 7
// January 20, 2009: DKW, Initial working version
//=================================================================================

#define python_module_name "bcp"

#include <Python.h>

#if PY_MAJOR_VERSION >= 3
#   define IS_PY3K
#endif

#ifdef IS_PY3K
#   define PyString_AsStringAndSize PyBytes_AsStringAndSize
#endif

#ifndef IS_PY3K
#   ifndef PyVarObject_HEAD_INIT
#       define PyVarObject_HEAD_INIT(type, size) PyObject_HEAD_INIT(type) size,
#   endif
#endif

#include <structmember.h>

// Python 2.6+ prefixes WRITE_RESTRICTED with PY_ 
#ifndef WRITE_RESTRICTED
#   define WRITE_RESTRICTED PY_WRITE_RESTRICTED
#endif

#include <sybfront.h>
#include <sybdb.h>

#include <string.h>
#include <stdio.h>

//=================================================================================
//     Optional debugging definitions, allows logging TDS events to a file
//=================================================================================
extern int tdsdump_open(const char *filename);

//=================================================================================
//                        Global dblib initialisation flag
//=================================================================================
static int DBAPI_Initialised = 0; // FreeTDS library must be initialized only once

//=================================================================================
//                           Declare exception types
//=================================================================================
static PyObject* BCP_InitialiseError;
static PyObject* BCP_ParameterError;
static PyObject* BCP_SessionError;
static PyObject* BCP_LoginError;
static PyObject* BCP_DataError;
static PyObject* BCP_DblibError;

//=================================================================================
//                           Instantiate exception types
//=================================================================================
static void declare_exceptions(PyObject* module)
{
    BCP_InitialiseError = PyErr_NewException("bcp.InitialiseError", NULL, NULL);
    Py_INCREF(BCP_InitialiseError);
    PyModule_AddObject(module, "InitialiseError", BCP_InitialiseError);

    BCP_ParameterError = PyErr_NewException("bcp.ParameterError", NULL, NULL);
    Py_INCREF(BCP_ParameterError);
    PyModule_AddObject(module, "ParameterError", BCP_ParameterError);

    BCP_LoginError = PyErr_NewException("bcp.LoginError", NULL, NULL);
    Py_INCREF(BCP_LoginError);
    PyModule_AddObject(module, "LoginError", BCP_LoginError);

    BCP_SessionError = PyErr_NewException("bcp.SessionError", NULL, NULL);
    Py_INCREF(BCP_SessionError);
    PyModule_AddObject(module, "SessionError", BCP_SessionError);

    BCP_DblibError = PyErr_NewException("bcp.DblibError", NULL, NULL);
    Py_INCREF(BCP_DblibError);
    PyModule_AddObject(module, "DblibError", BCP_DblibError);

    BCP_DataError = PyErr_NewException("bcp.DataError", NULL, NULL);
    Py_INCREF(BCP_DataError);
    PyModule_AddObject(module, "DataError", BCP_DataError);
}

//=================================================================================
// Allow the user to specify the freetds interfaces file to use for looking up
// database hosts
//=================================================================================
static PyObject* python_bcp_use_interfaces(PyObject* self, PyObject* args)
{
    char *filename = "/etc/freetds/freetds.conf";

    if (!PyArg_ParseTuple(args, "s", &filename))
    {
        PyErr_SetString(BCP_ParameterError, "Invalid file name passwd to use_interfaces()");
        return NULL;
    }

    dbsetifile(filename);
    Py_INCREF(Py_None);
    return Py_None;
}

//=================================================================================
//                              TDS Logging
//=================================================================================
static PyObject* python_bcp_logging(PyObject* self, PyObject* args, PyObject* kwargs)
{
    static char *keywords[] = {"filename", NULL};
    const char *filename = NULL;

    if (!PyArg_ParseTupleAndKeywords(args, kwargs, "|s", keywords, &filename))
    {
        PyErr_SetString(BCP_ParameterError, "Invalid|incomplete parameters passed to bcp logging()");
        return NULL;
    }

    if (filename != NULL) // Turn logging on and write to expressed filename
    {
        tdsdump_open(filename);
    }

    Py_INCREF(Py_None);
    return Py_None;
}
//=================================================================================
//                           Error and Message handling
//=================================================================================
static int bcp_message_handler(DBPROCESS* dbproc, DBINT msgno, int msgstate, int severity, char *msgtext, char *srvname, char *procname, int line)
{
    static char latest_message[2048];

    enum
    {
        changed_database = 5701,
        changed_language = 5703
    };

    if (msgno == changed_database || msgno == changed_language)
    {
        return 0;
    }

    if (severity < 1)
    {
        return 0;
    }

    snprintf
    (
        latest_message,
        sizeof(latest_message) - 1,
        "(Severity %d) %s",
        severity,
        msgtext
    );

    if (!PyErr_Occurred())
    {
        PyErr_SetString(BCP_DblibError, latest_message);
    }

    return (0);
}

static int bcp_error_handler(DBPROCESS* dbproc, int severity, int dberr, int oserr, char* dberrstr, char* oserrstr)
{
    static char latest_message[2048];
    void* preserved = dberrhandle(NULL); // Disable error handling within this callback

    if (dberr == SYBESMSG || severity < 1 || ! dberr || dberr == 156)
    {
        return(INT_CANCEL);
    }
    else if (PyErr_Occurred())
    {
        return(INT_CANCEL);
    }

    snprintf
    (
        latest_message,
        sizeof(latest_message) - 1,
        "(Severity %d) %s",
        severity,
        dberrstr
    );

    PyErr_SetString(BCP_DblibError, latest_message);
    dberrhandle(preserved);
    return(INT_CANCEL);
}

//=================================================================================
//                        Type and Object declarations
//=================================================================================
typedef struct
{
    PyObject_HEAD
    DBPROCESS* dbproc;
    Py_ssize_t batchsize;
    Py_ssize_t batchrows;
    Py_ssize_t rowsize;
    Py_ssize_t rowcount;
    Py_ssize_t textsize;
} BCP_ConnectionObject;

//=================================================================================
//                       Database connection methods
//=================================================================================
static PyObject* python_bcp_object_disconnect(BCP_ConnectionObject* self, PyObject* args)
{
    if (self && self->dbproc)
    {
        dbclose(self->dbproc);
        self->dbproc = NULL;
    }

    Py_INCREF(Py_None);
    return Py_None;
}

static PyObject* python_bcp_object_connect(BCP_ConnectionObject* self, PyObject* args, PyObject* kwargs)
{
    static char *keywords[] = {"server", "username", "password", "database", "batchsize", "textsize", NULL};

    const char *server = "hostname";
    const char *username = "dkw";
    const char *password = "";
    const char *database = "gps_querydb";

    char query[1024];

    LOGINREC *login;

    python_bcp_object_disconnect(self, Py_None);

    if (!PyArg_ParseTupleAndKeywords(args, kwargs, "s|sssii", keywords, &server, &username, &password, &database, &self->batchsize, &self->textsize))
    {
        PyErr_SetString(BCP_ParameterError, "Invalid|incomplete parameters passed to connect()");
        return NULL;
    }

    if ((login = dblogin()) == NULL)
    {
        return NULL;
    }

    DBSETLUSER(login, username);
    DBSETLPWD(login, password);
    BCP_SETL(login, 1); // Enable BCP on the connection

    self->dbproc = dbopen(login, server);
    dbloginfree(login);

    if (self->dbproc == NULL)
    {
        if (!PyErr_Occurred())
        {
            PyErr_SetString(BCP_LoginError, "failed connecting to server");
        }

        return NULL;
    }

    dbuse(self->dbproc, database);

    if (PyErr_Occurred())
    {
        return NULL;
    }

    snprintf(query, sizeof(query), "set textsize %d", (int) self->textsize);
    dbfcmd(self->dbproc, query);
    dbsqlexec(self->dbproc);
    while (dbresults(self->dbproc) != NO_MORE_RESULTS);

    /*
    snprintf(query, sizeof(query), "set quoted_identifier on");
    dbfcmd(self->dbproc, query);
    dbsqlexec(self->dbproc);
    while (dbresults(self->dbproc) != NO_MORE_RESULTS);
    */

    Py_INCREF(Py_None);

    return Py_None;
}

//=================================================================================
//      Begin a bcp data transfer "session" for a database table
//=================================================================================
static PyObject* python_bcp_object_session_init(BCP_ConnectionObject* self, PyObject* args)
{
    const char *table_name;

    if (!PyArg_ParseTuple(args, "s", &table_name))
    {
        PyErr_SetString(BCP_ParameterError, "Invalid table name passed to session init");
        return NULL;
    }

    if (bcp_init(self->dbproc, table_name, NULL, NULL, DB_IN) == FAIL)
    {
        PyErr_SetString(BCP_SessionError, "failed to create bcp session for the specified table");
        return NULL;
    }

    Py_INCREF(Py_None);
    return Py_None;
}

//=================================================================================
//               Allow user to set control parameters for bcp session
//=================================================================================
static PyObject* python_bcp_object_session_control(BCP_ConnectionObject* self, PyObject* args)
{
    Py_ssize_t field;
    Py_ssize_t value;

    if (!PyArg_ParseTuple(args, "ii", &field, &value))
    {
        PyErr_SetString(BCP_ParameterError, "bcp_control needs valid integer field specifier and value");
        return NULL;
    }

    bcp_control(self->dbproc, field, value);
    Py_INCREF(Py_None);
    return Py_None;
}

//=================================================================================
// Iterate through list of field values for a single row of bcp values, then
// send the row to the database server
//=================================================================================
static PyObject* python_bcp_object_sendrow(BCP_ConnectionObject* self, PyObject* args)
{
    static unsigned char* nullstr = (unsigned char*) "";

    PyObject* row_list;
    Py_ssize_t item_count;
    Py_ssize_t index;

    unsigned char** field_data = NULL;
    Py_ssize_t *field_sizes = NULL;

    if (!PyArg_ParseTuple(args, "O", &row_list))
    {
        PyErr_SetString(BCP_ParameterError, "Invalid column data passed to sendrow()");
    }
    else if (!PyList_Check(row_list))
    {
        PyErr_SetString(PyExc_ValueError, "Must use a list for sendrow()");
    }
    else if ((item_count = PyList_Size(row_list)) < (self->rowsize ? self->rowsize : 1))
    {
        PyErr_SetString(PyExc_ValueError, "Can only sendrow() using non-zero length lists of the same size");
    }
    else if (self->rowsize == 0)
    {
        self->rowsize = item_count;
    }

    if (PyErr_Occurred()) {
        return NULL;
    }
    
    if ((field_data = (unsigned char**) malloc(self->rowsize * sizeof(unsigned char*))) == NULL)
    {
        PyErr_SetString(BCP_DataError, "Couldn't allocate column field data storage");
    }
    else if ((field_sizes = (Py_ssize_t*) malloc(self->rowsize * sizeof(Py_ssize_t))) == NULL)
    {
        PyErr_SetString(BCP_DataError, "Couldn't allocate column field data storage");
    }

    if (PyErr_Occurred())
    {
        if (field_sizes != NULL) free(field_sizes);
        if (field_data != NULL) free(field_data);
        return NULL;
    }

    memset(field_data, 0, self->rowsize * sizeof(field_data[0]));
    memset(field_sizes, 0, self->rowsize * sizeof(field_sizes[0]));
    
    for (index = 0; index < self->rowsize && ! PyErr_Occurred(); ++index)
    {
        int bcp_column_position = index + 1; // Column position starts at 1
        int bind_as_null = 0;

        PyObject* item = PyList_GetItem(row_list, index);
        PyObject* str = NULL;

        if (item == NULL) // Invalid object raises an error
        {
            PyErr_SetString(BCP_DataError, "Could not retrieve value from list");
        }
        else if (item == Py_None) // If item is Python None object, then just write NULL column
        {
            bind_as_null = 1;
        }
        else if ((str = PyObject_Str(item)) == NULL)
        {
            PyErr_SetString(BCP_DataError, "Couldn't get copy of column data");
        }
        else
        {
            char *ptr;

            if (PyString_AsStringAndSize(str, &ptr, &field_sizes[index]) == -1)
            {
                PyErr_SetString(BCP_DataError, "Couldn't get details from column source");
            }
            else if ((field_data[index] = (unsigned char*) malloc(field_sizes[index] + 1)) == NULL)
            {
                PyErr_SetString(BCP_DataError, "Couldn't allocate column data buffer");
            }
            else
            {
                memset(field_data[index], 0, field_sizes[index] + 1);
                memcpy(field_data[index], ptr, field_sizes[index]);
            }

            Py_XDECREF(str);
        }

        if (! PyErr_Occurred())
        {
            if (bind_as_null)
            {
                if (self->rowcount == 0) // Must bind before first sendrow
                {
                    if (bcp_bind(self->dbproc, nullstr, 0, 0, nullstr, 0, SYBVARCHAR, bcp_column_position) == FAIL)
                    {
                        PyErr_SetString(BCP_DataError, "Can't bind column as NULL");
                    }
                }
                else
                {
                    if (bcp_colptr(self->dbproc, nullstr, bcp_column_position) == FAIL) // Map to NULL data
                    {
                        PyErr_SetString(BCP_DataError, "call to bcp_colptr() for null column failed");
                    }
                    else if (bcp_collen(self->dbproc, 0, bcp_column_position) == FAIL) // Map to zero length
                    {
                        PyErr_SetString(BCP_DataError, "call to bcp_collen() for null column failed");
                    }
                }
            }
            else if (field_data[index])
            {
                unsigned char* column_data = (unsigned char*) field_data[index];
                int column_width = field_sizes[index]; // strlen((char*) column_data);

                if (self->rowcount == 0) // Must bind before first sendrow
                {
                    if (bcp_bind(self->dbproc, column_data, 0, column_width, nullstr, 0, SYBVARCHAR, bcp_column_position) == FAIL)
                    {
                        PyErr_SetString(BCP_DataError, "call to bcp_bind() failed");
                    }
                }
                else
                {
                    // fprintf(stderr, ",[%s]", column_data);

                    if (bcp_colptr(self->dbproc, column_data, bcp_column_position) == FAIL)
                    {
                        PyErr_SetString(BCP_DataError, "call to bcp_colptr() column as string failed");
                    }
                    else if (bcp_collen(self->dbproc, column_width, bcp_column_position) == FAIL)
                    {
                        PyErr_SetString(BCP_DataError, "call to bcp_collen() for column as string failed");
                    }
                }
            }
            else
            {
                PyErr_SetString(BCP_DataError, "Couldn't get copy of column data");
            }
        }
    }

    // fprintf(stderr, "\n");
    // fflush(stderr);

    if (! PyErr_Occurred())
    {
        if (bcp_sendrow(self->dbproc) == FAIL && ! PyErr_Occurred())
        {
            PyErr_SetString(BCP_DataError, "Failed during bcp_sendrow()");
        }
        else if (self->batchsize == 0 || ++self->batchrows < self->batchsize)
        {
            // Don't do bcp_batch until we hit batchsize
            // Remember, batchsize can be changed by the client, so don't
            // try to use modulo arithmetic on rowcount instead
        }
        else if (bcp_batch(self->dbproc) == -1)
        {
            PyErr_SetString(BCP_DataError, "Failed during bcp_batch()");
        }
        else
        {
            self->batchrows = 0;
        }
    }

    // ============================================
    // Free up the fields used for the sendrow data
    // ============================================
    for (index = 0; index < self->rowsize; ++index)
    {
        if (field_data[index] != NULL)
        {
            free(field_data[index]);
        }
    }

    if (field_sizes != NULL) free(field_sizes);
    if (field_data != NULL) free(field_data);

    if (PyErr_Occurred())
    {
        return NULL;
    }

    ++self->rowcount;
    Py_INCREF(Py_None);
    return Py_None;
}

//=================================================================================
//  Flush rows written with sendrow and commit transaction, then end bcp session
//=================================================================================
static PyObject* python_bcp_object_done(BCP_ConnectionObject* self, PyObject* args)
{
    return Py_BuildValue("i", bcp_done(self->dbproc));
}

// //=================================================================================
// //   This method is unused and only exists to test the freetds connection
// //=================================================================================
// static PyObject* python_bcp_object_simple_query(BCP_ConnectionObject* self, PyObject* args, PyObject* kwargs)
// {
//     static char *keywords[] = {"query", "print_results", NULL};
//     int print_results = 0;
//     char* query = "select name from sysobjects";
// 
//     int result = PyArg_ParseTupleAndKeywords(args, kwargs, "s|B", keywords, &query, &print_results);
// 
//     if (! result)
//     {
//         PyErr_SetString(BCP_ParameterError, "No query passed to simplequery()");
//         return NULL;
//     }
// 
//     dbfcmd(self->dbproc, query);
//     dbsqlexec(self->dbproc);
// 
//     if (PyErr_Occurred())
//     {
//         return NULL;
//     }
// 
//     while (dbresults(self->dbproc) != NO_MORE_RESULTS)
//     {
//         int columns = dbnumcols(self->dbproc);
// 
//         if (columns > 0 && print_results)
//         {
//             char fields[columns][8192];
//             int nulls[columns];
//             int row = 0;
//             int index;
// 
//             printf("row");
// 
//             for (index = 0; index < columns; ++index)
//             {
//                 dbbind(self->dbproc, index + 1, NTBSTRINGBIND, sizeof(fields[index]), (BYTE *)fields[index]);
//                 dbnullbind(self->dbproc, index + 1, &nulls[index]);
//                 printf (",%s", dbcolname(self->dbproc, index + 1));
//             }
// 
//             printf ("\n");
// 
//             while (dbnextrow(self->dbproc) != NO_MORE_ROWS)
//             {
//                 printf("%d", row++);
// 
//                 for (index = 0; index < columns; ++index)
//                 {
//                     printf(",");
// 
//                     if (nulls[index] == -1)
//                         printf ("null");
//                     else
//                         printf ("\"%s\"", fields [index]);
//                 }
// 
//                 printf ("\n");
//             }
//         }
// 
//         if (PyErr_Occurred())
//         {
//             return NULL;
//         }
//     }
// 
//     Py_INCREF(Py_None);
//     return Py_None;
// }

//=================================================================================
//  Create a connection object and create a live connection to a database server
//=================================================================================
static int python_bcp_object_init(BCP_ConnectionObject* self, PyObject* args, PyObject* kwargs)
{
    if (! DBAPI_Initialised) // Initialise the database api if it hasn't been done already
    {
        if (dbinit() == FAIL)
        {
            PyErr_SetString(BCP_InitialiseError, "failed in dbinit()");
            return -1;
        }

        dbmsghandle(bcp_message_handler);
        dberrhandle(bcp_error_handler);
        DBAPI_Initialised = 1;
    }

    if (python_bcp_object_connect(self, args, kwargs) == NULL)
    {
        return -1;
    }

    return 0;
}

//=================================================================================
//                     Constructors and Destructors
//=================================================================================
static PyObject* python_bcp_object_new(PyTypeObject* type, PyObject* args, PyObject* kwargs)
{
    BCP_ConnectionObject* self = (BCP_ConnectionObject*)type->tp_alloc(type, 0);

    if (self)
    {
        self->textsize = 16777216;
        self->batchsize = 0;
        self->batchrows = 0;
        self->rowcount = 0;
        self->rowsize = 0;
        self->dbproc = NULL;
    }

    return (PyObject*) self;
}

static void python_bcp_object_delete(BCP_ConnectionObject* self)
{
    python_bcp_object_disconnect(self, Py_None);
    // NIL
    Py_TYPE(self)->tp_free(self);
    //self->ob_type->tp_free((PyObject*) self);
}

//=================================================================================
//                      Method declaration table for the module
//=================================================================================
static PyMethodDef python_bcp_methods[] = {
    {"use_interfaces", (PyCFunction)python_bcp_use_interfaces, METH_VARARGS|METH_KEYWORDS, "Select interfaces file to use"},
    {"logging", (PyCFunction)python_bcp_logging, METH_VARARGS|METH_KEYWORDS, "Start or stop logging"},
    {NULL, NULL, 0, NULL}        /* Sentinel */
};

//=================================================================================
//                 Method declaration table for the connection object
//=================================================================================
static PyMethodDef python_bcp_object_methods[] =
{
    {"connect", (PyCFunction)python_bcp_object_connect, METH_KEYWORDS, "Connect to server"},
    {"disconnect", (PyCFunction)python_bcp_object_disconnect, METH_VARARGS, "Disconnect from server"},
    {"init", (PyCFunction)python_bcp_object_session_init, METH_VARARGS|METH_KEYWORDS, "Prepare to bulk copy a specified table"},
    {"send", (PyCFunction)python_bcp_object_sendrow, METH_VARARGS|METH_KEYWORDS, "Commit transaction of rowcount sent and terminate bulk operation"},
    {"commit", (PyCFunction)python_bcp_object_done, METH_VARARGS, "Commit transaction of rowcount sent and terminate bulk operation"},
    {"done", (PyCFunction)python_bcp_object_done, METH_VARARGS, "Commit transaction of rowcount sent and terminate bulk operation"},
//    {"simplequery", (PyCFunction)python_bcp_object_simple_query, METH_VARARGS|METH_KEYWORDS, "(DEBUG_ONLY) Test connection with a simple query"},
    {"control", (PyCFunction)python_bcp_object_session_control, METH_VARARGS, "Change control parameters for bcp session"},
    {NULL}        /* Sentinel */
};

//=================================================================================
//             Member declaration table for the connection object
//=================================================================================
static PyMemberDef python_bcp_object_members[] =
{
    {"dbproc", T_UINT, offsetof(BCP_ConnectionObject, dbproc), READONLY, "dbproc"},
    {"rowcount", T_UINT, offsetof(BCP_ConnectionObject, rowcount), READONLY, "rows written so far"},
    {"batchsize", T_UINT, offsetof(BCP_ConnectionObject, batchsize), WRITE_RESTRICTED, "number of rows to write before a commit"},
    {"textsize", T_UINT, offsetof(BCP_ConnectionObject, textsize), WRITE_RESTRICTED, "maximum size of column data"},
    {NULL}        /* Sentinel */
};

//=================================================================================
//             Type definition structure for the connection object
//=================================================================================
static PyTypeObject BCP_ConnectionType = {
    PyVarObject_HEAD_INIT(NULL, 0)
    "bcp.Connection",             /*tp_name*/
    sizeof(BCP_ConnectionObject),             /*tp_basicsize*/
    0,                         /*tp_itemsize*/
    (destructor)python_bcp_object_delete, /*tp_dealloc*/
    0,                         /*tp_print*/
    0,                         /*tp_getattr*/
    0,                         /*tp_setattr*/
    0,                         /*tp_compare*/
    0,                         /*tp_repr*/
    0,                         /*tp_as_number*/
    0,                         /*tp_as_sequence*/
    0,                         /*tp_as_mapping*/
    0,                         /*tp_hash */
    0,                         /*tp_call*/
    0,                         /*tp_str*/
    0,                         /*tp_getattro*/
    0,                         /*tp_setattro*/
    0,                         /*tp_as_buffer*/
    Py_TPFLAGS_DEFAULT | Py_TPFLAGS_BASETYPE, /*tp_flags*/
    "BCP Connection object",           /* tp_doc */
    0,                         /* tp_traverse */
    0,                         /* tp_clear */
    0,                         /* tp_richcompare */
    0,                         /* tp_weaklistoffset */
    0,                         /* tp_iter */
    0,                         /* tp_iternext */
    python_bcp_object_methods, /* tp_methods */
    python_bcp_object_members, /* tp_members */
    0,                         /* tp_getset */
    0,                         /* tp_base */
    0,                         /* tp_dict */
    0,                         /* tp_descr_get */
    0,                         /* tp_descr_set */
    0,                         /* tp_dictoffset */
    (initproc)python_bcp_object_init,      /* tp_init */
    0,                         /* tp_alloc */
    python_bcp_object_new,     /* tp_new */
};

//=================================================================================
//               Python 3 requires new initialization pattern
//=================================================================================
#ifdef IS_PY3K
    static struct PyModuleDef module_definition = {
        PyModuleDef_HEAD_INIT,
        python_module_name,   /* m_name */
        python_bcp_moduledoc, /* m_doc */
        -1,                   /* m_size */
        python_bcp_methods,   /* m_methods */
        NULL,                 /* m_reload */
        NULL,                 /* m_traverse */
        NULL,                 /* m_clear */
        NULL,                 /* m_free */
    };
#endif

//=================================================================================
// Naming convention of init<module_name>. That's how python knows what to call
//=================================================================================
#ifdef IS_PY3K
#   define CREATE_MODULE() PyModule_Create(&module_definition)
#   define INIT_ERROR() return NULL
#   define INIT_SUCCESS() return module
#else
#   define CREATE_MODULE() Py_InitModule3(python_module_name, python_bcp_methods, python_bcp_moduledoc)
#   define INIT_ERROR() return
#   define INIT_SUCCESS() return
#endif

PyMODINIT_FUNC
#ifdef IS_PY3K
PyInit_bcp(void)
#else
initbcp(void)
#endif
{
    PyObject* module;

    if (PyType_Ready(&BCP_ConnectionType) < 0)
    {
        INIT_ERROR();
    }

    if ((module = CREATE_MODULE()) == NULL)
    {
        INIT_ERROR();
    }

    declare_exceptions(module);
    Py_INCREF(&BCP_ConnectionType);
    PyModule_AddObject(module, "Connection", (PyObject*) &BCP_ConnectionType);

    INIT_SUCCESS();
}
