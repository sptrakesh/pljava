/*
 * Copyright (c) 2003, 2004 TADA AB - Taby Sweden
 * Distributed under the terms shown in the file COPYRIGHT.
 * 
 * @author Thomas Hallgren
 */
#include <postgres.h>
#include <executor/spi.h>

#include "org_postgresql_pljava_internal_SPITupleTable.h"
#include "pljava/Exception.h"
#include "pljava/type/Type_priv.h"
#include "pljava/type/Tuple.h"
#include "pljava/type/TupleDesc.h"
#include "pljava/type/SPITupleTable.h"

static Type      s_SPITupleTable;
static TypeClass s_SPITupleTableClass;
static jclass    s_SPITupleTable_class;
static jmethodID s_SPITupleTable_init;

/*
 * org.postgresql.pljava.type.Tuple type.
 */
jobject SPITupleTable_create(JNIEnv* env, SPITupleTable* tts)
{
	jobject jtts;
	if(tts == 0)
		return 0;

	jtts = NativeStruct_obtain(env, tts);
	if(jtts == 0)
	{
		jtts = PgObject_newJavaObject(env, s_SPITupleTable_class, s_SPITupleTable_init);
		NativeStruct_init(env, jtts, tts);
	}
	return jtts;
}

static jvalue _SPITupleTable_coerceDatum(Type self, JNIEnv* env, Datum arg)
{
	jvalue result;
	result.l = SPITupleTable_create(env, (SPITupleTable*)DatumGetPointer(arg));
	return result;
}

static Type SPITupleTable_obtain(Oid typeId)
{
	return s_SPITupleTable;
}

/* Make this datatype available to the postgres system.
 */
extern Datum SPITupleTable_initialize(PG_FUNCTION_ARGS);
PG_FUNCTION_INFO_V1(SPITupleTable_initialize);
Datum SPITupleTable_initialize(PG_FUNCTION_ARGS)
{
	JNINativeMethod methods[] = {
		{
		"_getCount",
	  	"()I",
	  	Java_org_postgresql_pljava_internal_SPITupleTable__1getCount
		},
		{
		"_getSlot",
		"(I)Lorg/postgresql/pljava/internal/Tuple;",
		Java_org_postgresql_pljava_internal_SPITupleTable__1getSlot
		},
		{
		"_getTupleDesc",
		"()Lorg/postgresql/pljava/internal/TupleDesc;",
		Java_org_postgresql_pljava_internal_SPITupleTable__1getTupleDesc
		},
		{
		"_invalidate",
	  	"()V",
	  	Java_org_postgresql_pljava_internal_SPITupleTable__1invalidate
		},
		{ 0, 0, 0 }};

	JNIEnv* env = (JNIEnv*)PG_GETARG_POINTER(0);

	s_SPITupleTable_class = (*env)->NewGlobalRef(
				env, PgObject_getJavaClass(env, "org/postgresql/pljava/internal/SPITupleTable"));

	PgObject_registerNatives2(env, s_SPITupleTable_class, methods);

	s_SPITupleTable_init = PgObject_getJavaMethod(
				env, s_SPITupleTable_class, "<init>", "()V");

	s_SPITupleTableClass = NativeStructClass_alloc("type.SPITupleTable");
	s_SPITupleTableClass->JNISignature   = "Lorg/postgresql/pljava/internal/SPITupleTable;";
	s_SPITupleTableClass->javaTypeName   = "org.postgresql.pljava.internal.SPITupleTable";
	s_SPITupleTableClass->coerceDatum    = _SPITupleTable_coerceDatum;
	s_SPITupleTable = TypeClass_allocInstance(s_SPITupleTableClass, InvalidOid);

	Type_registerJavaType("org.postgresql.pljava.internal.SPITupleTable", SPITupleTable_obtain);
	PG_RETURN_VOID();
}

/****************************************
 * JNI methods
 ****************************************/
/*
 * Class:     org_postgresql_pljava_internal_SPITupleTable
 * Method:    _getCount
 * Signature: ()I
 */
JNIEXPORT jint JNICALL
Java_org_postgresql_pljava_internal_SPITupleTable__1getCount(JNIEnv* env, jobject _this)
{
	SPITupleTable* tupleTable;
	PLJAVA_ENTRY_FENCE(0)
	tupleTable = (SPITupleTable*)NativeStruct_getStruct(env, _this);
	if(tupleTable == 0)
		return 0;
	
	return (jint)(tupleTable->alloced - tupleTable->free);
}

/*
 * Class:     org_postgresql_internal_pljava_SPITupleTable
 * Method:    _getSlot
 * Signature: (I)Lorg/postgresql/pljava/internal/Tuple;
 */
JNIEXPORT jobject JNICALL
Java_org_postgresql_pljava_internal_SPITupleTable__1getSlot(JNIEnv* env, jobject _this, jint pos)
{
	SPITupleTable* tupleTable;
	PLJAVA_ENTRY_FENCE(0)
	tupleTable = (SPITupleTable*)NativeStruct_getStruct(env, _this);
	if(tupleTable == 0)
		return 0;

	if(pos < 0 || pos >= (tupleTable->alloced - tupleTable->free))
		return 0;

	return Tuple_create(env, tupleTable->vals[pos]);
}

/*
 * Class:     org_postgresql_pljava_internal_SPITupleTable
 * Method:    _getTupleDesc
 * Signature: ()Lorg/postgresql/pljava/internal/TupleDesc;
 */
JNIEXPORT jobject JNICALL
Java_org_postgresql_pljava_internal_SPITupleTable__1getTupleDesc(JNIEnv* env, jobject _this)
{
	SPITupleTable* tupleTable;
	PLJAVA_ENTRY_FENCE(0)
	tupleTable = (SPITupleTable*)NativeStruct_getStruct(env, _this);
	if(tupleTable == 0)
		return 0;

	return TupleDesc_create(env, tupleTable->tupdesc);
}

/*
 * Class:     org_postgresql_pljava_internal_SPITupleTable
 * Method:    _invalidate
 * Signature: ()V
 */
JNIEXPORT void JNICALL
Java_org_postgresql_pljava_internal_SPITupleTable__1invalidate(JNIEnv* env, jobject _this)
{
	SPITupleTable* tupleTable;
	PLJAVA_ENTRY_FENCE_VOID
	tupleTable = (SPITupleTable*)NativeStruct_releasePointer(env, _this);
	if(tupleTable != 0)
		SPI_freetuptable(tupleTable);
}
