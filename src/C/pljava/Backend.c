/*
 * This file contains software that has been made available under
 * The Mozilla Public License 1.1. Use and distribution hereof are
 * subject to the restrictions set forth therein.
 *
 * Copyright (c) 2003 TADA AB - Taby Sweden
 * All Rights Reserved
 */
#include <postgres.h>
#include <executor/spi.h>
#include <commands/trigger.h>
#include <utils/elog.h>
#include <utils/guc.h>
#include <fmgr.h>
#include <access/heapam.h>
#include <utils/syscache.h>
#include <catalog/pg_proc.h>
#include <catalog/pg_type.h>
#include <storage/ipc.h>
#include <stdio.h>
#include <ctype.h>

#include "pljava/Function.h"
#include "pljava/type/Type.h"
#include "pljava/type/NativeStruct.h"
#include "pljava/HashMap.h"
#include "pljava/Exception.h"
#include "pljava/Backend_JNI.h"
#include "pljava/type/String.h"

/* Example format: "/usr/local/pgsql/lib" */
#ifndef PKGLIBDIR
#error "PKGLIBDIR needs to be defined to compile this file."
#endif

DECLARE_MUTEX(jvmInitMutex)
pthread_t pljava_mainThread;
bool elogErrorOccured;

#define LOCAL_REFERENCE_COUNT 32

static JNIEnv* s_mainEnv = 0;
static JavaVM* s_javaVM = 0;

static void initJavaVM(JNIEnv* env)
{
	Datum envDatum = PointerGetDatum(env);
	DirectFunctionCall1(Exception_initialize, envDatum);
	DirectFunctionCall1(Type_initialize, envDatum);
	DirectFunctionCall1(Function_initialize, envDatum);
}

static Datum callFunction(JNIEnv* env, PG_FUNCTION_ARGS)
{
	sigjmp_buf saveRestart;

	Datum retval;

	/* Since the call does not originate from the JavaVM, we must
	 * push a local frame that ensures garbage collection of
	 * new objecs once popped (somewhat similar to palloc, but for
	 * Java objects).
	 */
	if((*env)->PushLocalFrame(env, LOCAL_REFERENCE_COUNT) < 0)
	{
		/* Out of memory
		 */
		(*env)->ExceptionClear(env);
		ereport(ERROR, (
			errcode(ERRCODE_OUT_OF_MEMORY),
			errmsg("Unable to create java frame for local references")));
	}

	/* Push a new "try/catch" block.
	 */
	bool saveErrorOccured = elogErrorOccured;
	elogErrorOccured = false;
	memcpy(&saveRestart, &Warn_restart, sizeof(saveRestart));
	if(sigsetjmp(Warn_restart, 1) != 0)
	{
		/* Catch block.
		 */
		memcpy(&Warn_restart, &saveRestart, sizeof(Warn_restart));
		elogErrorOccured = saveErrorOccured;
		NativeStruct_expireAll(env);
		(*env)->PopLocalFrame(env, 0);
		siglongjmp(Warn_restart, 1);
	}

	Oid funcOid = fcinfo->flinfo->fn_oid;
	if(CALLED_AS_TRIGGER(fcinfo))
	{
		/* Called as a trigger procedure
		 */
		Function function = Function_getFunction(env, funcOid, true);
		retval = Function_invokeTrigger(function, env, fcinfo);
	}
	else
	{
		/* Called as a function
		 */
		Function function = Function_getFunction(env, funcOid, false);
		retval = Function_invoke(function, env, fcinfo);
	}
	Exception_checkException(env);
	if(elogErrorOccured)
		longjmp(Warn_restart, 1);

	/* Pop of the "try/catch" block.
	 */
	memcpy(&Warn_restart, &saveRestart, sizeof(Warn_restart));
	elogErrorOccured = saveErrorOccured;
	NativeStruct_expireAll(env);
	(*env)->PopLocalFrame(env, 0);
	return retval;
}

bool pljavaEntryFence(JNIEnv* env)
{
	if(elogErrorOccured)
	{
		Exception_elogErrorException(env);
		return true;
	}
	THREAD_FENCE(true)
	return false;
}

/**
 * Special purpose logging function called from JNI when verbose is enabled.
 */
static jint JNICALL my_vfprintf(FILE* fp, const char* format, va_list args)
{
    char buf[1024];
    vsnprintf(buf, sizeof(buf), format, args);

    /* Trim off trailing newline and other whitespace.
     */
    char* bp = buf;
    char* ep = bp + strlen(bp) - 1;
    while(ep >= bp && isspace(*ep))
 		--ep;
 	++ep;
 	*ep = 0;

    elog(INFO, buf);
    return 0;
}

/*
 * Append those parts of path that has not yet been appended. The HashMap unique is
 * keeping track of what has been appended already. First appended part will be
 * prefixed with prefix.
 */
static void appendPathParts(const char* path, StringInfoData* bld, HashMap unique, const char* prefix)
{
	if(path == 0 || strlen(path) == 0)
		return;

	StringInfoData buf;
	for (;;)
	{
		if(*path == 0)
			break;

		size_t len = strcspn(path, ";:");

		if(len == 1 && *(path+1) == ':' && isalnum(*path))
			/*
			 * Windows drive designator, leave it "as is".
			 */
			len = strcspn(path+2, ";:") + 2;
		else
		if(len == 0)
			{
			/* Ignore zero length components.
			 */
			++path;
			continue;
			}

		initStringInfo(&buf);

#ifdef CYGWIN
		/**
		 * Translate "/cygdrive/<driverLetter>/" into "<driveLetter>:/" since
		 * the JVM dynamic loader will not recognize the former.
		 * 
		 * This is somewhat ugly and will be removed as soon as the native port
		 * of postgresql is released.
		 */
		if(len >= 11
		&& (*path == '/' || *path == '\\')
		&& strncmp(path + 1, "cygdrive", 8) == 0)
		{
			const char* cp = path + 9;
			if(*cp == '/' || *cp == '\\')
			{
				++cp;
				char driveLetter = *cp;
				if(isalnum(driveLetter))
				{
					++cp;
					char sep = *cp;
					if(sep == '/' || sep == '\\' || sep == ':' || sep == ';' || sep == 0)
					{
						/* Path starts with /cygdrive/<driveLetter>. Replace
						 * this with <driverLetter>:\
						 */
						appendStringInfoChar(&buf, driveLetter);
						appendStringInfo(&buf, ":\\");
						if(sep == '\\' || sep == '/')
							++cp;
						len -= (cp - path);
						path = cp;
					}
				}
			}
		}				
#endif

		if(*path == '$')
		{
			if(len == 7 || (strcspn(path, "/\\") == 7 && strncmp(path, "$libdir", 7) == 0))
			{
				len -= 7;
				path += 7;
				appendStringInfo(&buf, PKGLIBDIR);
			}
			else
				ereport(ERROR, (
					errcode(ERRCODE_INVALID_NAME),
					errmsg("invalid macro name '%*s' in dynamic library path", len, path)));
		}

		if(len > 0)
		{
			appendBinaryStringInfo(&buf, path, len);
			path += len;
		}

		char* part = buf.data;
		if(HashMap_getByString(unique, part) == 0)
		{
			if(HashMap_size(unique) == 0)
				appendStringInfo(bld, prefix);
			else
#if WIN32 || CYGWIN
				appendStringInfoChar(bld, ';');
#else
				appendStringInfoChar(bld, ':');
#endif
			appendStringInfo(bld, part);
			HashMap_putByString(unique, part, (void*)1);
		}
		pfree(part);
		if(*path == 0)
			break;
		++path; /* Skip ':' */
	}
}

/*
 * Get the Dynamic_library_path configuration parameter and the
 * LD_LIBRARY_PATH (or PATH in case of WIN32) environment variable
 * merged together. The components found in the Dynamic_library_path
 * are placed first in the result. Substitute for any macros appearing
 * in the given string. Result is always freshly palloc'd.
 *
 * NOTE Currently, we only allow the $libdir macro. All else will
 * result in an exception.
 */
static char* getLibraryPath(const char* prefix)
{
	StringInfoData buf;
	initStringInfo(&buf);

	HashMap unique = HashMap_create(13, CurrentMemoryContext);
	const char* dynPath = GetConfigOption("dynamic_library_path");
	if(dynPath != 0)
		appendPathParts(dynPath, &buf, unique, prefix);

#if WIN32 || CYGWIN
	appendPathParts(getenv("PATH"), &buf, unique, prefix); /* DLL's are found using standard system path */
#else
	appendPathParts(getenv("LD_LIBRARY_PATH"), &buf, unique, prefix);
#endif

	PgObject_free((PgObject)unique);
	char* path = buf.data;
	if(strlen(path) == 0)
	{
		pfree(path);
		path = 0;
	}
	return path;
}

/*
 * Get the CLASSPATH. Result is always freshly palloc'd.
 */
static char* getClassPath(const char* prefix)
{
	const char* p = getenv("CLASSPATH");
	if(p == 0 || strlen(p) == 0)
		return 0;

	StringInfoData buf;
	initStringInfo(&buf);
	appendStringInfo(&buf, prefix);
	appendStringInfo(&buf, p);
	return buf.data;
}

/*
 * proc_exit callback to tear down the JVM
 */
static void _destroyJavaVM(int status, Datum dummy)
{
	if(s_javaVM != 0)
	{
		elog(LOG, "Destroying JavaVM");
		(*s_javaVM)->DestroyJavaVM(s_javaVM);
		s_javaVM = 0;
	}
}

static void initializeJavaVM()
{
	BEGIN_CRITICAL(jvmInitMutex)
	if(s_javaVM != 0)
	{
		END_CRITICAL(jvmInitMutex)
		return;
	}

	int nOptions = 0;
	JavaVMOption options[10]; /* increase if more options are needed! */
	
	DirectFunctionCall1(HashMap_initialize, 0);

	char* classPath = getClassPath("-Djava.class.path=");
	if(classPath != 0)
	{
		options[nOptions].optionString = classPath;
		options[nOptions].extraInfo = 0;
		nOptions++;
	}

	/**
	 * The JVM needs the java.library.path to find its way back to
	 * the loaded module.
	 */
	char* dynLibPath = getLibraryPath("-Djava.library.path=");
	if(dynLibPath != 0)
	{
		options[nOptions].optionString = dynLibPath;
		options[nOptions].extraInfo = 0;
		nOptions++;
	}

	/**
	 * Default LoggingManager initializer.
	 */
	options[nOptions].optionString = "-Djava.util.logging.config.class=org.postgresql.pljava.internal.LoggerConfigurator";
	options[nOptions].extraInfo = 0;
	nOptions++;

	/**
	 * As stipulated by JRT-2003
	 */
	options[nOptions].optionString = "-Dsqlj.defaultconnection=jdbc:default:connection";
	options[nOptions].extraInfo = 0;
	nOptions++;

	options[nOptions].optionString = "vfprintf";
	options[nOptions].extraInfo = (void*)my_vfprintf;
	nOptions++;

	JavaVMInitArgs vm_args;
	vm_args.nOptions = nOptions;
	vm_args.options  = options;
	vm_args.version  = JNI_VERSION_1_4;
	vm_args.ignoreUnrecognized = JNI_TRUE;

	elog(LOG, "Creating JavaVM");
	if(JNI_CreateJavaVM(&s_javaVM, (void **)&s_mainEnv, &vm_args) != JNI_OK)
		ereport(ERROR, (errmsg("Failed to create Java VM")));

	if(dynLibPath != 0)
		pfree(dynLibPath);
	if(classPath != 0)
		pfree(classPath);

	/* Register an on_proc_exit handler that destroys the VM
	 */
	on_proc_exit(_destroyJavaVM, 0);

#ifdef USE_THREADS
	pljava_mainThread = pthread_self();
#endif
	initJavaVM(s_mainEnv);
	END_CRITICAL(jvmInitMutex)
}

extern Datum java_call_handler(PG_FUNCTION_ARGS);
PG_FUNCTION_INFO_V1(java_call_handler);

/*
 * This is the entry point for all calls. The java_call_handler must be
 * defined in plsql like this:
 *
 */
Datum java_call_handler(PG_FUNCTION_ARGS)
{
	if(s_javaVM == 0)
		initializeJavaVM();

#ifdef USE_THREADS
	if(!pthread_equal(pthread_self(), pljava_mainThread))
		ereport(ERROR, (errmsg("Multiple threads call in to java_call_handler")));
#endif

	SPI_connect();
	Datum retval = callFunction(s_mainEnv, fcinfo);
	SPI_finish();
	return retval;
}

/****************************************
 * JNI methods
 ****************************************/
JNIEXPORT jint JNICALL JNI_OnLoad(JavaVM* vm, void* reserved)
{
	return JNI_VERSION_1_4;
}

/*
 * Class:     org_postgresql_pljava_internal_Backend
 * Method:    getConfigOption
 * Signature: (Ljava/lang/String;)Ljava/lang/String;
 */
JNIEXPORT jstring JNICALL
Java_org_postgresql_pljava_internal_Backend_getConfigOption(JNIEnv* env, jclass cls, jstring jkey)
{
	THREAD_FENCE(0)
	char* key = String_createNTS(env, jkey);
	if(key == 0)
		return 0;

	const char* value = GetConfigOption(key);
	pfree(key);
	return (value == 0) ? 0 : String_createJavaStringFromNTS(env, value);
}

/*
 * Class:     org_postgresql_pljava_internal_Backend
 * Method:    log
 * Signature: (ILjava/lang/String;)V
 */
JNIEXPORT void JNICALL
Java_org_postgresql_pljava_internal_Backend_log(JNIEnv* env, jclass cls, jint logLevel, jstring jstr)
{
	THREAD_FENCE_VOID
	char* str = String_createNTS(env, jstr);
	if(str == 0)
		return;
	elog(logLevel, str);
	pfree(str);
}	

/*
 * Class:     org_postgresql_pljava_internal_Backend
 * Method:    isBackendThread
 * Signature: ()Z
 */
JNIEXPORT jboolean JNICALL
Java_org_postgresql_pljava_internal_Backend_isBackendThread(JNIEnv* env, jclass cls)
{
	return IS_MAIN_THREAD ? JNI_TRUE : JNI_FALSE;
}