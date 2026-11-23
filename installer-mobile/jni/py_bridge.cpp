#include <jni.h>
#include <mutex>
#include <string>
#include <vector>
#include <android/log.h>
#include <unistd.h>
#include <Python.h>
#include "wii_extractor.h"
#include "yaz0.h"

#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, "CookerBridge", __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, "CookerBridge", __VA_ARGS__)

static JavaVM* gVm = nullptr;
static jobject gCallbackGlobal = nullptr;
static jmethodID gOnLogMethod = nullptr;
static jmethodID gOnProgressMethod = nullptr;

extern "C" JNIEXPORT jint JNICALL JNI_OnLoad(JavaVM* vm, void* reserved) {
    gVm = vm;
    return JNI_VERSION_1_6;
}

static void sendLogToJava(const char* text) {
    if (!gVm || !gCallbackGlobal || !gOnLogMethod) return;
    JNIEnv* env = nullptr;
    bool attached = false;
    if (gVm->GetEnv((void**)&env, JNI_VERSION_1_6) != JNI_OK) {
        if (gVm->AttachCurrentThread(&env, nullptr) == JNI_OK) {
            attached = true;
        }
    }
    if (env) {
        jstring jText = env->NewStringUTF(text ? text : "");
        env->CallVoidMethod(gCallbackGlobal, gOnLogMethod, jText);
        env->DeleteLocalRef(jText);
        if (attached) {
            gVm->DetachCurrentThread();
        }
    }
}

static void sendProgressToJava(int percent, const char* text) {
    if (!gVm || !gCallbackGlobal || !gOnProgressMethod) return;
    JNIEnv* env = nullptr;
    bool attached = false;
    if (gVm->GetEnv((void**)&env, JNI_VERSION_1_6) != JNI_OK) {
        if (gVm->AttachCurrentThread(&env, nullptr) == JNI_OK) {
            attached = true;
        }
    }
    if (env) {
        jstring jText = env->NewStringUTF(text ? text : "");
        env->CallVoidMethod(gCallbackGlobal, gOnProgressMethod, percent, jText);
        env->DeleteLocalRef(jText);
        if (attached) {
            gVm->DetachCurrentThread();
        }
    }
}

static PyObject* py_log_wrapper(PyObject* self, PyObject* args) {
    const char* str = nullptr;
    if (PyArg_ParseTuple(args, "s", &str)) {
        if (str && strlen(str) > 0) {
            sendLogToJava(str);
        }
    }
    Py_RETURN_NONE;
}

static PyObject* py_progress_wrapper(PyObject* self, PyObject* args) {
    int percent = 0;
    const char* str = nullptr;
    if (PyArg_ParseTuple(args, "is", &percent, &str)) {
        if (str) {
            sendProgressToJava(percent, str);
        }
    }
    Py_RETURN_NONE;
}

static PyObject* py_yaz0_decompress(PyObject* self, PyObject* args) {
    Py_buffer view;
    if (!PyArg_ParseTuple(args, "y*", &view)) {
        return NULL;
    }
    const uint8_t* data = (const uint8_t*)view.buf;
    size_t len = (size_t)view.len;
    if (len < 16 || memcmp(data, "Yaz0", 4) != 0) {
        PyObject* ret = PyBytes_FromStringAndSize((const char*)data, len);
        PyBuffer_Release(&view);
        return ret;
    }
    uint32_t decompSize = ((uint32_t)data[4] << 24) | ((uint32_t)data[5] << 16) | ((uint32_t)data[6] << 8) | (uint32_t)data[7];
    PyObject* outObj = PyBytes_FromStringAndSize(NULL, decompSize);
    if (!outObj) {
        PyBuffer_Release(&view);
        return PyErr_NoMemory();
    }
    char* dst = PyBytes_AS_STRING(outObj);
    long long n = yaz0_decode(data, len, (uint8_t*)dst, decompSize);
    PyBuffer_Release(&view);
    if (n != (long long)decompSize) {
        Py_DECREF(outObj);
        PyErr_SetString(PyExc_ValueError, "bad Yaz0 stream");
        return NULL;
    }
    return outObj;
}

static PyMethodDef BridgeMethods[] = {
    {"log_bridge", py_log_wrapper, METH_VARARGS, "Send logs to Android UI"},
    {"progress_bridge", py_progress_wrapper, METH_VARARGS, "Send progress to Android UI"},
    {"yaz0_decompress", py_yaz0_decompress, METH_VARARGS, "Decompress Yaz0 natively in C"},
    {NULL, NULL, 0, NULL}
};

static struct PyModuleDef bridge_module = {
    PyModuleDef_HEAD_INIT,
    "android_bridge",
    NULL,
    -1,
    BridgeMethods
};

PyMODINIT_FUNC PyInit_android_bridge(void) {
    return PyModule_Create(&bridge_module);
}

// ---------------------------------------------------------------------------
// JNI Method: isWiiDisc(String filePath)
// ---------------------------------------------------------------------------
extern "C" JNIEXPORT jboolean JNICALL
Java_com_galaxy_installer_CookerEngine_isWiiDisc(
        JNIEnv *env, jclass clazz, jstring jFilePath) {
    if (!jFilePath) return JNI_FALSE;
    const char* filePath = env->GetStringUTFChars(jFilePath, nullptr);
    bool result = wii::isWiiDiscImage(filePath);
    env->ReleaseStringUTFChars(jFilePath, filePath);
    return result ? JNI_TRUE : JNI_FALSE;
}

// ---------------------------------------------------------------------------
// JNI Method: extractDisc(String imagePath, String outDir, boolean includeMovies, Callback)
// ---------------------------------------------------------------------------
extern "C" JNIEXPORT jint JNICALL
Java_com_galaxy_installer_CookerEngine_extractDisc(
        JNIEnv *env, jclass clazz,
        jstring jImagePath, jstring jOutDir,
        jboolean jIncludeMovies, jobject jCallback) {

    const char* imagePath = env->GetStringUTFChars(jImagePath, nullptr);
    const char* outDir = env->GetStringUTFChars(jOutDir, nullptr);
    bool includeMovies = (bool)jIncludeMovies;

    gCallbackGlobal = env->NewGlobalRef(jCallback);
    jclass cbClass = env->GetObjectClass(jCallback);
    gOnLogMethod = env->GetMethodID(cbClass, "onLog", "(Ljava/lang/String;)V");
    gOnProgressMethod = env->GetMethodID(cbClass, "onProgress", "(ILjava/lang/String;)V");

    bool success = wii::extractWiiDisc(
        imagePath, outDir, includeMovies,
        [](const std::string& msg) {
            sendLogToJava(msg.c_str());
        },
        [](int pct, const std::string& msg) {
            sendProgressToJava(pct, msg.c_str());
        }
    );

    env->ReleaseStringUTFChars(jImagePath, imagePath);
    env->ReleaseStringUTFChars(jOutDir, outDir);
    env->DeleteGlobalRef(gCallbackGlobal);
    gCallbackGlobal = nullptr;

    return success ? 0 : 1;
}

// ---------------------------------------------------------------------------
// Shared embedded Python initialization & helpers
// ---------------------------------------------------------------------------
static std::mutex sPyMutex;
static std::wstring sPyHome;
static bool sPyInited = false;

static std::string escapePyLiteral(const char* raw) {
    std::string out;
    if (!raw) return out;
    for (const char* p = raw; *p; ++p) {
        char c = *p;
        if (c == '\\') out += "\\\\";
        else if (c == '\'') out += "\\'";
        else if (c == '\n') out += "\\n";
        else if (c == '\r') out += "\\r";
        else out += c;
    }
    return out;
}

static void ensurePythonInitializedLocked(const char* pythonHome, const char* scriptsDir) {
    if (!sPyInited) {
        sendLogToJava("[Native] Initializing embedded Python engine...");
        Py_DontWriteBytecodeFlag = 1;
        PyImport_AppendInittab("android_bridge", PyInit_android_bridge);

        wchar_t* wHome = Py_DecodeLocale(pythonHome, nullptr);
        if (wHome) {
            sPyHome.assign(wHome);
            PyMem_RawFree(wHome);
            Py_SetPythonHome(const_cast<wchar_t*>(sPyHome.c_str()));
        }

        Py_Initialize();
        PyEval_SaveThread();
        sPyInited = true;
        sendLogToJava("[Native] Python engine initialized.");
    }

    PyGILState_STATE gstate = PyGILState_Ensure();
    std::string escScripts = escapePyLiteral(scriptsDir);
    std::string setupScript =
        "import sys, os, android_bridge\n"
        "_scripts_dir = '" + escScripts + "'\n"
        "if _scripts_dir not in sys.path:\n"
        "    sys.path.insert(0, _scripts_dir)\n"
        "class OutputLogger:\n"
        "    def write(self, s):\n"
        "        for line in s.splitlines():\n"
        "            if line.strip():\n"
        "                android_bridge.log_bridge(line.strip())\n"
        "    def flush(self):\n"
        "        pass\n"
        "sys.stdout = OutputLogger()\n"
        "sys.stderr = OutputLogger()\n";
    PyRun_SimpleString(setupScript.c_str());
    PyGILState_Release(gstate);
}

static void logAndClearPyError() {
    PyObject *ptype = nullptr, *pvalue = nullptr, *ptraceback = nullptr;
    PyErr_Fetch(&ptype, &pvalue, &ptraceback);
    if (pvalue) {
        PyObject *pStr = PyObject_Str(pvalue);
        if (pStr) {
            const char *errText = PyUnicode_AsUTF8(pStr);
            if (errText) {
                sendLogToJava(("[Python Error] " + std::string(errText)).c_str());
            }
            Py_DECREF(pStr);
        }
        Py_DECREF(pvalue);
    }
    if (ptype) Py_DECREF(ptype);
    if (ptraceback) Py_DECREF(ptraceback);
    PyErr_Clear();
}

extern "C" JNIEXPORT jint JNICALL
Java_com_galaxy_installer_CookerEngine_runCooker(
        JNIEnv *env, jclass clazz,
        jstring jPythonHome, jstring jScriptsDir,
        jstring jInputDir, jstring jOutputDir,
        jboolean jWithMovies, jobject jCallback) {

    std::lock_guard<std::mutex> lock(sPyMutex);

    const char* pythonHome = env->GetStringUTFChars(jPythonHome, nullptr);
    const char* scriptsDir = env->GetStringUTFChars(jScriptsDir, nullptr);
    const char* inputDir = env->GetStringUTFChars(jInputDir, nullptr);
    const char* outputDir = env->GetStringUTFChars(jOutputDir, nullptr);
    bool withMovies = (bool)jWithMovies;

    gCallbackGlobal = env->NewGlobalRef(jCallback);
    jclass cbClass = env->GetObjectClass(jCallback);
    gOnLogMethod = env->GetMethodID(cbClass, "onLog", "(Ljava/lang/String;)V");
    gOnProgressMethod = env->GetMethodID(cbClass, "onProgress", "(ILjava/lang/String;)V");
    env->DeleteLocalRef(cbClass);

    ensurePythonInitializedLocked(pythonHome, scriptsDir);

    PyGILState_STATE gstate = PyGILState_Ensure();
    std::string escInput = escapePyLiteral(inputDir);
    std::string escOutput = escapePyLiteral(outputDir);

    // Execute cook.py conversion
    std::string runScript =
        "import cook, android_bridge, traceback\n"
        "def on_progress(done, total):\n"
        "    pct = int(done * 100 / total) if total > 0 else 0\n"
        "    android_bridge.progress_bridge(pct, f'Cooking assets: {done}/{total} ({pct}%)')\n"
        "try:\n"
        "    print('[Engine] Beginning conversion...')\n"
        "    ret = cook.convert(\n"
        "        data_dir='" + escInput + "',\n"
        "        out_dir='" + escOutput + "',\n"
        "        with_movies=" + (withMovies ? "True" : "False") + ",\n"
        "        progress=on_progress\n"
        "    )\n"
        "    if ret != 0:\n"
        "        raise RuntimeError(f'cook.convert returned non-zero code {ret}')\n"
        "    print('[Engine] Cooking completed successfully!')\n"
        "except BaseException as e:\n"
        "    import traceback\n"
        "    print('[ERROR] ' + str(e))\n"
        "    traceback.print_exc()\n"
        "    raise\n";

    sendLogToJava("[Native] Invoking cook.convert()...");
    int result = PyRun_SimpleString(runScript.c_str());
    if (result != 0) {
        logAndClearPyError();
    }

    PyGILState_Release(gstate);

    env->ReleaseStringUTFChars(jPythonHome, pythonHome);
    env->ReleaseStringUTFChars(jScriptsDir, scriptsDir);
    env->ReleaseStringUTFChars(jInputDir, inputDir);
    env->ReleaseStringUTFChars(jOutputDir, outputDir);
    env->DeleteGlobalRef(gCallbackGlobal);
    gCallbackGlobal = nullptr;

    return result;
}

extern "C" JNIEXPORT jstring JNICALL
Java_com_galaxy_installer_CookerEngine_inspectRiivolution(
        JNIEnv *env, jclass clazz,
        jstring jPythonHome, jstring jScriptsDir, jstring jInputDir) {

    std::lock_guard<std::mutex> lock(sPyMutex);

    const char* pythonHome = env->GetStringUTFChars(jPythonHome, nullptr);
    const char* scriptsDir = env->GetStringUTFChars(jScriptsDir, nullptr);
    const char* inputDir = env->GetStringUTFChars(jInputDir, nullptr);

    ensurePythonInitializedLocked(pythonHome, scriptsDir);

    PyGILState_STATE gstate = PyGILState_Ensure();
    std::string resultJson = "{\"packs\":[]}";

    PyObject* pModName = PyUnicode_FromString("riivolution");
    PyObject* pMod = PyImport_Import(pModName);
    Py_DECREF(pModName);
    if (pMod) {
        PyObject* pReload = PyImport_ReloadModule(pMod);
        if (pReload) {
            Py_DECREF(pMod);
            pMod = pReload;
        }
        PyObject* pFunc = PyObject_GetAttrString(pMod, "inspect_source");
        if (pFunc && PyCallable_Check(pFunc)) {
            PyObject* pArgs = Py_BuildValue("(s)", inputDir);
            PyObject* pRes = PyObject_CallObject(pFunc, pArgs);
            Py_DECREF(pArgs);
            if (pRes) {
                PyObject* pJsonMod = PyImport_ImportModule("json");
                if (pJsonMod) {
                    PyObject* pDumps = PyObject_GetAttrString(pJsonMod, "dumps");
                    if (pDumps) {
                        PyObject* pJsonStr = PyObject_CallFunctionObjArgs(pDumps, pRes, nullptr);
                        if (pJsonStr) {
                            const char* utf = PyUnicode_AsUTF8(pJsonStr);
                            if (utf) resultJson = utf;
                            Py_DECREF(pJsonStr);
                        }
                        Py_DECREF(pDumps);
                    }
                    Py_DECREF(pJsonMod);
                }
                Py_DECREF(pRes);
            } else {
                PyErr_Clear();
            }
            Py_DECREF(pFunc);
        } else {
            Py_XDECREF(pFunc);
            PyErr_Clear();
        }
        Py_DECREF(pMod);
    } else {
        PyErr_Clear();
    }

    PyGILState_Release(gstate);
    env->ReleaseStringUTFChars(jPythonHome, pythonHome);
    env->ReleaseStringUTFChars(jScriptsDir, scriptsDir);
    env->ReleaseStringUTFChars(jInputDir, inputDir);
    return env->NewStringUTF(resultJson.c_str());
}

extern "C" JNIEXPORT jint JNICALL
Java_com_galaxy_installer_CookerEngine_runRiivolutionCooker(
        JNIEnv *env, jclass clazz,
        jstring jPythonHome, jstring jScriptsDir,
        jstring jInputDir, jstring jOutputDir,
        jstring jPackId, jstring jOptionsJson, jobject jCallback) {

    std::lock_guard<std::mutex> lock(sPyMutex);

    const char* pythonHome = env->GetStringUTFChars(jPythonHome, nullptr);
    const char* scriptsDir = env->GetStringUTFChars(jScriptsDir, nullptr);
    const char* inputDir = env->GetStringUTFChars(jInputDir, nullptr);
    const char* outputDir = env->GetStringUTFChars(jOutputDir, nullptr);
    const char* packId = jPackId ? env->GetStringUTFChars(jPackId, nullptr) : "";
    const char* optionsJson = jOptionsJson ? env->GetStringUTFChars(jOptionsJson, nullptr) : "{}";

    gCallbackGlobal = env->NewGlobalRef(jCallback);
    jclass cbClass = env->GetObjectClass(jCallback);
    gOnLogMethod = env->GetMethodID(cbClass, "onLog", "(Ljava/lang/String;)V");
    gOnProgressMethod = env->GetMethodID(cbClass, "onProgress", "(ILjava/lang/String;)V");
    env->DeleteLocalRef(cbClass);

    ensurePythonInitializedLocked(pythonHome, scriptsDir);

    PyGILState_STATE gstate = PyGILState_Ensure();
    std::string escInput = escapePyLiteral(inputDir);
    std::string escOutput = escapePyLiteral(outputDir);
    std::string escPackId = escapePyLiteral(packId);
    std::string escOpts = escapePyLiteral(optionsJson);

    std::string runScript =
        "import importlib, riivolution, android_bridge, traceback\n"
        "importlib.reload(riivolution)\n"
        "def on_progress(done, total):\n"
        "    pct = int(done * 100 / total) if total > 0 else 0\n"
        "    android_bridge.progress_bridge(pct, f'Cooking mod files: {done}/{total} ({pct}%)')\n"
        "try:\n"
        "    print('[Riivolution] Preparing mod overlay...')\n"
        "    _pid = '" + escPackId + "' or None\n"
        "    _opts = '" + escOpts + "' or '{}'\n"
        "    res = riivolution.install_from_dir(\n"
        "        source_dir='" + escInput + "',\n"
        "        out_data_dir='" + escOutput + "',\n"
        "        pack_id=_pid,\n"
        "        options_json=_opts,\n"
        "        progress=on_progress\n"
        "    )\n"
        "    for item in res:\n"
        "        print(f\"[Riivolution] Installed '{item['name']}' ({item['files_cooked']} files, {item['created_files']} new)\")\n"
        "except BaseException as e:\n"
        "    print('[ERROR] ' + str(e))\n"
        "    traceback.print_exc()\n"
        "    raise\n";

    int result = PyRun_SimpleString(runScript.c_str());
    if (result != 0) {
        logAndClearPyError();
    }

    PyGILState_Release(gstate);

    env->ReleaseStringUTFChars(jPythonHome, pythonHome);
    env->ReleaseStringUTFChars(jScriptsDir, scriptsDir);
    env->ReleaseStringUTFChars(jInputDir, inputDir);
    env->ReleaseStringUTFChars(jOutputDir, outputDir);
    if (jPackId) env->ReleaseStringUTFChars(jPackId, packId);
    if (jOptionsJson) env->ReleaseStringUTFChars(jOptionsJson, optionsJson);
    env->DeleteGlobalRef(gCallbackGlobal);
    gCallbackGlobal = nullptr;

    return result;
}
