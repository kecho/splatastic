#define PY_SSIZE_T_CLEAN
#include <Python.h>
#include <files/IFileSystem.h>
#include <tasks/ITaskSystem.h>
#include <scene/SceneDb.h>
#include <string>

#define KW_FN(pyname, fn_name, desc) \
    { #pyname, (PyCFunction)(fn_name), METH_VARARGS | METH_KEYWORDS, desc }

using namespace splatastic;

IFileSystem* g_fs = nullptr;
ITaskSystem* g_ts = nullptr;
SceneDb* g_sdb = nullptr;
PyObject* g_exObj = nullptr;

PyObject* initialize(PyObject* self, PyObject* vargs, PyObject* kwds)
{
    if (g_ts == nullptr)
    {
        TaskSystemDesc tsDesc;
        g_ts = ITaskSystem::create(tsDesc);
        g_ts->start();
    }

    if (g_fs == nullptr)
    {
        FileSystemDesc fsDesc = { g_ts };
        g_fs = IFileSystem::create(fsDesc);
    }

    if (g_sdb == nullptr)
        g_sdb = new SceneDb(*g_fs, *g_ts);
    
    Py_RETURN_NONE;
}

PyObject* shutdown(PyObject* self, PyObject* vargs, PyObject* kwds)
{
    if (g_sdb != nullptr)
    {
        delete g_sdb;
        g_sdb = nullptr;
    }

    if (g_fs != nullptr)
    {
        delete g_fs;
        g_fs = nullptr;
    }

    if (g_ts != nullptr)
    {
        g_ts->signalStop();
        g_ts->join();
        delete g_ts;
        g_ts = nullptr;
    }

    Py_RETURN_NONE;
}

// python wrapper objects

struct SceneAsyncRequest
{
    PyObject_HEAD;
    SceneLoadHandle loadHandle;
    Py_buffer destCopyPayloadView = {};
    std::string fileName;
};

int SceneAsyncRequest_init(PyObject* self, PyObject * vargs, PyObject* kwds)
{
    auto& sceneRequest = *(SceneAsyncRequest*)self;
    new (&sceneRequest) SceneAsyncRequest;

    static const char* keywords[] = { "file", nullptr };
    char* fileName = nullptr;
    if (!PyArg_ParseTupleAndKeywords(vargs, kwds, "s", const_cast<char**>(keywords), &fileName))
        return -1;

    sceneRequest.fileName = fileName;
    sceneRequest.loadHandle = g_sdb->openScene(fileName); 

    if (!sceneRequest.loadHandle.valid())
    {
        PyErr_SetString(g_exObj, "Count not open designated scene. There might be too many scenes open in flight.");
        return -1;
    }
    
    return 0;
}

PyObject* SceneAsyncRequest_resolve(PyObject* self, PyObject* vargs, PyObject* kwds)
{
    auto& sceneRequest = *(SceneAsyncRequest*)self;
    if (!sceneRequest.loadHandle.valid())
    {
        PyErr_SetString(g_exObj, "Invalid scene request.");
        return nullptr;
    }

    g_sdb->resolve(sceneRequest.loadHandle); 

    Py_RETURN_NONE;
}

PyObject* SceneAsyncRequest_status(PyObject* self, PyObject* vargs, PyObject* kwds)
{
    auto& sceneRequest = *(SceneAsyncRequest*)self;
    if (!sceneRequest.loadHandle.valid())
    {
        PyErr_SetString(g_exObj, "Invalid scene request.");
        return nullptr;
    }
    
    SceneLoadStatus loadStatus = g_sdb->checkStatus(sceneRequest.loadHandle);
    return Py_BuildValue("(is)", loadStatus, loadStatus == SceneLoadStatus::Failed ? g_sdb->errorStr(sceneRequest.loadHandle) : "");
}

PyObject* SceneAsyncRequest_ioProgress(PyObject* self, PyObject* vargs, PyObject* kwds)
{
    auto& sceneRequest = *(SceneAsyncRequest*)self;
    if (!sceneRequest.loadHandle.valid())
    {
        PyErr_SetString(g_exObj, "Invalid scene request.");
        return nullptr;
    }
    
    SceneLoadStatus loadStatus = g_sdb->checkStatus(sceneRequest.loadHandle);
    unsigned long long b = {}, sz = {};
    if (loadStatus == SceneLoadStatus::Reading)
    {
        g_sdb->ioProgress(sceneRequest.loadHandle, b, sz);
    }

    return Py_BuildValue("(KK)", b, sz);
}

PyObject* SceneAsyncRequest_requestCopyPayload(PyObject* self, PyObject* vargs, PyObject* kwds)
{
    auto& sceneRequest = *(SceneAsyncRequest*)self;
    if (!sceneRequest.loadHandle.valid())
    {
        PyErr_SetString(g_exObj, "Invalid scene request.");
        return nullptr;
    }

    if (sceneRequest.destCopyPayloadView.obj != nullptr)
    {
        PyErr_SetString(g_exObj, "Copy request already happening. Ensure to close the previous request before proceeding.");
        return nullptr;
    }

    const char* arguments[] = { "destination", nullptr };
    PyObject* destinationObj = nullptr;
    if (!PyArg_ParseTupleAndKeywords(vargs, kwds, "O", const_cast<char**>(arguments), &destinationObj))
        return nullptr;

    if (!PyObject_CheckBuffer(destinationObj))
    {
        PyErr_SetString(g_exObj, "Destination must be a buffer protocol object.");
        return nullptr;
    }

    sceneRequest.destCopyPayloadView = {};
    if (PyObject_GetBuffer(destinationObj, &sceneRequest.destCopyPayloadView, 0) < 0)
        return nullptr;

    if (!g_sdb->copyPayload(sceneRequest.loadHandle, (char*)sceneRequest.destCopyPayloadView.buf, sceneRequest.destCopyPayloadView.len))
    {
        PyBuffer_Release(&sceneRequest.destCopyPayloadView);
        PyErr_SetString(g_exObj, "Error trying to copy to payload, closing request.");
        return nullptr;
    }
        

    Py_RETURN_NONE;
}

PyObject* SceneAsyncRequest_closeCopyPayload(PyObject* self, PyObject* vargs, PyObject* kwds)
{
    auto& sceneRequest = *(SceneAsyncRequest*)self;
    if (!sceneRequest.loadHandle.valid())
    {
        PyErr_SetString(g_exObj, "Invalid scene request.");
        return nullptr;
    }

    if (sceneRequest.destCopyPayloadView.obj == nullptr)
    {
        PyErr_SetString(g_exObj, "Copy has not started. Ensure to start a request before proceeding.");
        return nullptr;
    }

    PyBuffer_Release(&sceneRequest.destCopyPayloadView);
    sceneRequest.destCopyPayloadView = {};
    Py_RETURN_NONE;
}

PyObject* SceneAsyncRequest_payloadSize(PyObject* self, PyObject* vargs, PyObject* kwds)
{
    auto& sceneRequest = *(SceneAsyncRequest*)self;
    if (!sceneRequest.loadHandle.valid())
    {
        PyErr_SetString(g_exObj, "Invalid scene request.");
        return nullptr;
    }

    if (!sceneRequest.loadHandle.valid())
    {
        PyErr_SetString(g_exObj, "Invalid load handle.");
        return nullptr;
    }

    return Py_BuildValue("i", g_sdb->payloadSize(sceneRequest.loadHandle));
}

PyObject* SceneAsyncRequest_metadata(PyObject* self, PyObject* vargs, PyObject* kwds)
{
    auto& sceneRequest = *(SceneAsyncRequest*)self;
    if (!sceneRequest.loadHandle.valid())
    {
        PyErr_SetString(g_exObj, "Invalid scene request.");
        return nullptr;
    }

    if (!sceneRequest.loadHandle.valid())
    {
        PyErr_SetString(g_exObj, "Invalid load handle.");
        return nullptr;
    }

    SplatSceneMetadata metadata = {};
    if (!g_sdb->sceneMetadata(sceneRequest.loadHandle, metadata))
    {
        PyErr_SetString(g_exObj, "Invalid scene metadata.");
        return nullptr;
    }

    return Py_BuildValue("(ii)", metadata.vertexCount, metadata.stride);
}

void SceneAsyncRequest_dealloc(PyObject* self)
{
    auto& sceneRequest = *(SceneAsyncRequest*)self;
    if (g_sdb != nullptr && sceneRequest.loadHandle.valid())
        g_sdb->closeScene(sceneRequest.loadHandle);
    if (sceneRequest.destCopyPayloadView.obj != nullptr)
        PyBuffer_Release(&sceneRequest.destCopyPayloadView);
    sceneRequest.~SceneAsyncRequest();
    Py_TYPE(self)->tp_free(self);
}

//module types
PyTypeObject g_SceneAsyncRequestType =
{
    PyVarObject_HEAD_INIT(NULL, 0)
};

bool registerSceneAsyncRequestType(PyObject* moduleObj)
{
    static PyMethodDef s_methods[] = {
        KW_FN(ioProgress, SceneAsyncRequest_ioProgress, "Returns: tuple (bytesRead, totalBytes)"),
        KW_FN(status, SceneAsyncRequest_status, "Gets the status [None, Reading, Invalidhandle, SuccessFinish, Failed] and a string message if an error exists"),
        KW_FN(resolve, SceneAsyncRequest_resolve, "Halts and blocks for io to finish."),
        KW_FN(request_copy_payload, SceneAsyncRequest_requestCopyPayload, "Copies a payload to a destination memory view."),
        KW_FN(payload_size, SceneAsyncRequest_payloadSize, "Gets payload size."),
        KW_FN(metadata, SceneAsyncRequest_metadata, "Gets metadata as a tuple."),
        KW_FN(close_copy_payload, SceneAsyncRequest_closeCopyPayload, "Closes an in flight copy payload."),
        { nullptr }
    };

    PyTypeObject& o = g_SceneAsyncRequestType;
    o.tp_name = "native.SceneAsyncRequest";
    o.tp_basicsize = sizeof(SceneAsyncRequest);
    o.tp_init = SceneAsyncRequest_init;
    o.tp_dealloc = SceneAsyncRequest_dealloc;
    o.tp_flags = Py_TPFLAGS_DEFAULT;
    o.tp_new = PyType_GenericNew;
    o.tp_methods = s_methods;
    o.tp_doc = R"(
    Scene Async Request Object.
    Constructor:
        file (str): file containing splat scene to load.
    )";

    if (PyType_Ready(&o) < 0)
        return false;

    if (PyModule_AddObject(moduleObj, "SceneAsyncRequest", (PyObject*)&o) < 0)
        return false;

    return true;
}

static PyMethodDef g_methods[] = {
    {"init", (PyCFunction)initialize, METH_VARARGS | METH_KEYWORDS, NULL},
    {"shutdown", (PyCFunction)shutdown, METH_VARARGS | METH_KEYWORDS, NULL},
    {NULL, NULL, 0, NULL},
};

static struct PyModuleDef moduleDesc = {
    PyModuleDef_HEAD_INIT,
    "native",
    NULL,
    -1,
    g_methods,
};

bool initializeTypes(PyObject* moduleObj)
{
    if (!registerSceneAsyncRequestType(moduleObj))
        return false;
    
    return true;
}

PyMODINIT_FUNC PyInit_native(void)
{
    PyObject* moduleObj = PyModule_Create(&moduleDesc);

    if (!initializeTypes(moduleObj))
    {
        Py_DECREF(moduleObj);
        return nullptr;
    }

    g_exObj = PyErr_NewException("native.exception", nullptr, nullptr);
    Py_XINCREF(g_exObj);

    if (PyModule_AddObject(moduleObj, "exception_object", g_exObj) < 0)
    {
        Py_XDECREF(g_exObj);
        Py_CLEAR(g_exObj);
        Py_DECREF(moduleObj);
        return nullptr;
    }

    return moduleObj;
}
