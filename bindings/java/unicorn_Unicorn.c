/*

Java bindings for the Unicorn Emulator Engine

Copyright(c) 2023 Robert Xiao

This program is free software; you can redistribute it and/or
modify it under the terms of the GNU General Public License
version 2 as published by the Free Software Foundation.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with this program; if not, write to the Free Software
Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301, USA.

*/

#include <sys/types.h>
#include "unicorn/platform.h"
#include <stdlib.h>
#include <string.h>

#include <unicorn/unicorn.h>
#include <unicorn/x86.h>
#include "unicorn_Unicorn.h"

static JavaVM *cachedJVM;

JNIEXPORT jint JNICALL JNI_OnLoad(JavaVM *jvm, void *reserved)
{
    cachedJVM = jvm;
    return JNI_VERSION_1_6;
}

static void throwUnicornException(JNIEnv *env, uc_err err)
{
    jclass clazz = (*env)->FindClass(env, "unicorn/UnicornException");
    const char *msg = uc_strerror(err);
    (*env)->ThrowNew(env, clazz, msg);
}

static void throwCustomUnicornException(JNIEnv *env, const char *msg)
{
    jclass clazz = (*env)->FindClass(env, "unicorn/UnicornException");
    (*env)->ThrowNew(env, clazz, msg);
}

static void throwOutOfMemoryError(JNIEnv *env, char *message)
{
    jclass clazz = (*env)->FindClass(env, "java/lang/OutOfMemoryError");
    (*env)->ThrowNew(env, clazz, message);
}

static jobject makeX86_MMR(JNIEnv *env, const uc_x86_mmr *mmr)
{
    if (mmr == NULL) {
        return NULL;
    }

    static jclass clazz;
    if (!clazz) {
        clazz = (*env)->FindClass(env, "unicorn/X86_MMR");
        if (!clazz)
            return NULL;
        clazz = (*env)->NewGlobalRef(env, clazz);
        if (!clazz)
            return NULL;
    }

    static jmethodID clazzInit;
    if (!clazzInit) {
        clazzInit = (*env)->GetMethodID(env, clazz, "<init>", "(JIIS)V");
        if (!clazzInit)
            return NULL;
    }

    return (*env)->NewObject(env, clazz, clazzInit, (jlong)mmr->base,
                             (jint)mmr->limit, (jint)mmr->flags,
                             (jshort)mmr->selector);
}

static jobject makeArm64_CP(JNIEnv *env, const uc_arm64_cp_reg *cp_reg)
{
    if (cp_reg == NULL) {
        return NULL;
    }

    static jclass clazz;
    if (!clazz) {
        clazz = (*env)->FindClass(env, "unicorn/Arm64_CP");
        if (!clazz)
            return NULL;
        clazz = (*env)->NewGlobalRef(env, clazz);
        if (!clazz)
            return NULL;
    }

    static jmethodID clazzInit;
    if (!clazzInit) {
        clazzInit = (*env)->GetMethodID(env, clazz, "<init>", "(IIIIIJ)V");
        if (!clazzInit)
            return NULL;
    }

    return (*env)->NewObject(env, clazz, clazzInit, (jint)cp_reg->crn,
                             (jint)cp_reg->crm, (jint)cp_reg->op0,
                             (jint)cp_reg->op1, (jint)cp_reg->op2,
                             (jlong)cp_reg->val);
}

static jobject makeTranslationBlock(JNIEnv *env, const uc_tb *tb)
{
    if (tb == NULL) {
        return NULL;
    }

    static jclass clazz;
    if (!clazz) {
        clazz = (*env)->FindClass(env, "unicorn/TranslationBlock");
        if (!clazz)
            return NULL;
        clazz = (*env)->NewGlobalRef(env, clazz);
        if (!clazz)
            return NULL;
    }

    static jmethodID clazzInit;
    if (!clazzInit) {
        clazzInit = (*env)->GetMethodID(env, clazz, "<init>", "(JII)V");
        if (!clazzInit)
            return NULL;
    }

    return (*env)->NewObject(env, clazz, clazzInit, (jlong)tb->pc,
                             (jint)tb->icount, (jint)tb->size);
}

struct hook_wrapper {
    uc_hook uc_hh;
    jobject unicorn;
    jobject hook_obj;
    jmethodID hook_meth;
    jobject user_data;
};

static bool hookErrorCheck(uc_engine *uc, JNIEnv *env)
{
    /* If a hook throws an exception, we want to report it as soon as possible.
    Additionally, once an exception is set, calling further hooks is
    inadvisable. Therefore, try and stop the emulator as soon as an exception
    is detected.
    */
    if ((*env)->ExceptionCheck(env)) {
        uc_emu_stop(uc);
        return true;
    }
    return false;
}

static const char *const sig_InterruptHook =
    "(Lunicorn/Unicorn;ILjava/lang/Object;)V";
static void cb_hookintr(uc_engine *uc, uint32_t intno, void *user_data)
{
    JNIEnv *env;
    (*cachedJVM)->AttachCurrentThread(cachedJVM, (void **)&env, NULL);
    struct hook_wrapper *hh = user_data;
    (*env)->CallVoidMethod(env, hh->hook_obj, hh->hook_meth, hh->unicorn,
                           (jint)intno, hh->user_data);
    hookErrorCheck(uc, env);
}

static const char *const sig_InHook =
    "(Lunicorn/Unicorn;IILjava/lang/Object;)I";
static uint32_t cb_insn_in(uc_engine *uc, uint32_t port, int size,
                           void *user_data)
{
    JNIEnv *env;
    (*cachedJVM)->AttachCurrentThread(cachedJVM, (void **)&env, NULL);
    struct hook_wrapper *hh = user_data;
    jint result =
        (*env)->CallIntMethod(env, hh->hook_obj, hh->hook_meth, hh->unicorn,
                              (jint)port, (jint)size, hh->user_data);
    if (hookErrorCheck(uc, env)) {
        return 0;
    }
    return (uint32_t)result;
}

static const char *const sig_OutHook =
    "(Lunicorn/Unicorn;IIILjava/lang/Object;)V";
static void cb_insn_out(uc_engine *uc, uint32_t port, int size, uint32_t value,
                        void *user_data)
{
    JNIEnv *env;
    (*cachedJVM)->AttachCurrentThread(cachedJVM, (void **)&env, NULL);
    struct hook_wrapper *hh = user_data;
    (*env)->CallVoidMethod(env, hh->hook_obj, hh->hook_meth, hh->unicorn,
                           (jint)port, (jint)size, (jint)value, hh->user_data);
    hookErrorCheck(uc, env);
}

static const char *const sig_SyscallHook =
    "(Lunicorn/Unicorn;Ljava/lang/Object;)V";
static void cb_insn_syscall(struct uc_struct *uc, void *user_data)
{
    JNIEnv *env;
    (*cachedJVM)->AttachCurrentThread(cachedJVM, (void **)&env, NULL);
    struct hook_wrapper *hh = user_data;
    (*env)->CallVoidMethod(env, hh->hook_obj, hh->hook_meth, hh->unicorn,
                           hh->user_data);
    hookErrorCheck(uc, env);
}

static const char *const sig_CpuidHook =
    "(Lunicorn/Unicorn;Ljava/lang/Object;)I";
static int cb_insn_cpuid(struct uc_struct *uc, void *user_data)
{
    JNIEnv *env;
    (*cachedJVM)->AttachCurrentThread(cachedJVM, (void **)&env, NULL);
    struct hook_wrapper *hh = user_data;
    jint result = (*env)->CallIntMethod(env, hh->hook_obj, hh->hook_meth,
                                        hh->unicorn, hh->user_data);
    if (hookErrorCheck(uc, env)) {
        return 0;
    }
    return (int)result;
}

static const char *const sig_Arm64SysHook =
    "(Lunicorn/Unicorn;ILunicorn/Arm64_CP;Ljava/lang/Object;)I";
static uint32_t cb_insn_sys(uc_engine *uc, uc_arm64_reg reg,
                            const uc_arm64_cp_reg *cp_reg, void *user_data)
{
    JNIEnv *env;
    (*cachedJVM)->AttachCurrentThread(cachedJVM, (void **)&env, NULL);
    struct hook_wrapper *hh = user_data;
    jobject jcp_reg = makeArm64_CP(env, cp_reg);
    if (!jcp_reg) {
        hookErrorCheck(uc, env);
        return 0;
    }
    jint result =
        (*env)->CallIntMethod(env, hh->hook_obj, hh->hook_meth, hh->unicorn,
                              (jint)reg, jcp_reg, hh->user_data);
    if (hookErrorCheck(uc, env)) {
        return 0;
    }
    return (uint32_t)result;
}

static const char *const sig_CodeHook =
    "(Lunicorn/Unicorn;JILjava/lang/Object;)V";
static void cb_hookcode(uc_engine *uc, uint64_t address, uint32_t size,
                        void *user_data)
{
    JNIEnv *env;
    (*cachedJVM)->AttachCurrentThread(cachedJVM, (void **)&env, NULL);
    struct hook_wrapper *hh = user_data;
    (*env)->CallVoidMethod(env, hh->hook_obj, hh->hook_meth, hh->unicorn,
                           (jlong)address, (jint)size, hh->user_data);
    hookErrorCheck(uc, env);
}

static const char *const sig_EventMemHook =
    "(Lunicorn/Unicorn;IJIJLjava/lang/Object;)Z";
static bool cb_eventmem(uc_engine *uc, uc_mem_type type, uint64_t address,
                        int size, int64_t value, void *user_data)
{
    JNIEnv *env;
    (*cachedJVM)->AttachCurrentThread(cachedJVM, (void **)&env, NULL);
    struct hook_wrapper *hh = user_data;
    jboolean result = (*env)->CallBooleanMethod(
        env, hh->hook_obj, hh->hook_meth, hh->unicorn, (jint)type,
        (jlong)address, (jint)size, (jlong)value, hh->user_data);
    if (hookErrorCheck(uc, env)) {
        return false;
    }
    return result != JNI_FALSE;
}

static const char *const sig_MemHook =
    "(Lunicorn/Unicorn;IJIJLjava/lang/Object;)V";
static void cb_hookmem(uc_engine *uc, uc_mem_type type, uint64_t address,
                       int size, int64_t value, void *user_data)
{
    JNIEnv *env;
    (*cachedJVM)->AttachCurrentThread(cachedJVM, (void **)&env, NULL);
    struct hook_wrapper *hh = user_data;
    (*env)->CallVoidMethod(env, hh->hook_obj, hh->hook_meth, hh->unicorn,
                           (jint)type, (jlong)address, (jint)size, (jlong)value,
                           hh->user_data);
    hookErrorCheck(uc, env);
}

static const char *const sig_InvalidInstructionHook =
    "(Lunicorn/Unicorn;Ljava/lang/Object;)Z";
static bool cb_hookinsn_invalid(uc_engine *uc, void *user_data)
{
    JNIEnv *env;
    (*cachedJVM)->AttachCurrentThread(cachedJVM, (void **)&env, NULL);
    struct hook_wrapper *hh = user_data;
    jboolean result = (*env)->CallBooleanMethod(
        env, hh->hook_obj, hh->hook_meth, hh->unicorn, hh->user_data);
    if (hookErrorCheck(uc, env)) {
        return false;
    }
    return result != JNI_FALSE;
}

static const char *const sig_EdgeGeneratedHook =
    "(Lunicorn/Unicorn;Lunicorn/TranslationBlock;"
    "Lunicorn/TranslationBlock;Ljava/lang/Object;)V";
static void cb_edge_gen(uc_engine *uc, uc_tb *cur_tb, uc_tb *prev_tb,
                        void *user_data)
{
    JNIEnv *env;
    (*cachedJVM)->AttachCurrentThread(cachedJVM, (void **)&env, NULL);
    struct hook_wrapper *hh = user_data;
    jobject jcur_tb = makeTranslationBlock(env, cur_tb);
    if (!jcur_tb) {
        hookErrorCheck(uc, env);
        return;
    }

    jobject jprev_tb = makeTranslationBlock(env, prev_tb);
    if (!jprev_tb) {
        hookErrorCheck(uc, env);
        return;
    }

    (*env)->CallVoidMethod(env, hh->hook_obj, hh->hook_meth, hh->unicorn,
                           jcur_tb, jprev_tb, hh->user_data);
    hookErrorCheck(uc, env);
}

static const char *const sig_TcgOpcodeHook =
    "(Lunicorn/Unicorn;JJJILjava/lang/Object;)V";
static void cb_tcg_op_2(uc_engine *uc, uint64_t address, uint64_t arg1,
                        uint64_t arg2, uint32_t size, void *user_data)
{
    JNIEnv *env;
    (*cachedJVM)->AttachCurrentThread(cachedJVM, (void **)&env, NULL);
    struct hook_wrapper *hh = user_data;
    (*env)->CallVoidMethod(env, hh->hook_obj, hh->hook_meth, hh->unicorn,
                           (jlong)address, (jlong)arg1, (jlong)arg2, (jint)size,
                           hh->user_data);
    hookErrorCheck(uc, env);
}

static const char *const sig_TlbFillHook =
    "(Lunicorn/Unicorn;JILjava/lang/Object;)J";
static bool cb_tlbevent(uc_engine *uc, uint64_t vaddr, uc_mem_type type,
                        uc_tlb_entry *entry, void *user_data)
{
    JNIEnv *env;
    (*cachedJVM)->AttachCurrentThread(cachedJVM, (void **)&env, NULL);
    struct hook_wrapper *hh = user_data;
    jlong result =
        (*env)->CallLongMethod(env, hh->hook_obj, hh->hook_meth, hh->unicorn,
                               (jlong)vaddr, (jint)type, hh->user_data);
    if (hookErrorCheck(uc, env)) {
        return false;
    }
    if (result == -1L) {
        return false;
    } else {
        entry->paddr = result & ~UC_PROT_ALL;
        entry->perms = result & UC_PROT_ALL;
        return true;
    }
}

static const char *const sig_MmioReadHandler =
    "(Lunicorn/Unicorn;JILjava/lang/Object;)J";
static uint64_t cb_mmio_read(uc_engine *uc, uint64_t offset, unsigned size,
                             void *user_data)
{
    JNIEnv *env;
    (*cachedJVM)->AttachCurrentThread(cachedJVM, (void **)&env, NULL);
    struct hook_wrapper *hh = user_data;
    jlong result =
        (*env)->CallLongMethod(env, hh->hook_obj, hh->hook_meth, hh->unicorn,
                               (jlong)offset, (jint)size, hh->user_data);
    if (hookErrorCheck(uc, env)) {
        return 0;
    }
    return (uint64_t)result;
}

static const char *const sig_MmioWriteHandler =
    "(Lunicorn/Unicorn;JIJLjava/lang/Object;)V";
static void cb_mmio_write(uc_engine *uc, uint64_t offset, unsigned size,
                          uint64_t value, void *user_data)
{
    JNIEnv *env;
    (*cachedJVM)->AttachCurrentThread(cachedJVM, (void **)&env, NULL);
    struct hook_wrapper *hh = user_data;
    (*env)->CallVoidMethod(env, hh->hook_obj, hh->hook_meth, hh->unicorn,
                           (jlong)offset, (jint)size, (jlong)value,
                           hh->user_data);
    hookErrorCheck(uc, env);
}

/*
 * Class:     unicorn_Unicorn
 * Method:    _open
 * Signature: (II)J
 */
JNIEXPORT jlong JNICALL Java_unicorn_Unicorn__1open(JNIEnv *env, jclass clazz,
                                                    jint arch, jint mode)
{
    uc_engine *eng = NULL;
    uc_err err = uc_open(arch, mode, &eng);
    if (err != UC_ERR_OK) {
        throwUnicornException(env, err);
        return 0;
    }
    return (jlong)eng;
}

/*
 * Class:     unicorn_Unicorn
 * Method:    _close
 * Signature: (J)V
 */
JNIEXPORT void JNICALL Java_unicorn_Unicorn__1close(JNIEnv *env, jclass clazz,
                                                    jlong uc)
{
    uc_err err = uc_close((uc_engine *)uc);
    if (err != UC_ERR_OK) {
        throwUnicornException(env, err);
        return;
    }
}

/*
 * Class:     unicorn_Unicorn
 * Method:    _emu_start
 * Signature: (JJJJJ)V
 */
JNIEXPORT void JNICALL Java_unicorn_Unicorn__1emu_1start(
    JNIEnv *env, jclass clazz, jlong uc, jlong begin, jlong until,
    jlong timeout, jlong count)
{
    uc_err err =
        uc_emu_start((uc_engine *)uc, begin, until, timeout, (size_t)count);
    if (err != UC_ERR_OK) {
        throwUnicornException(env, err);
        return;
    }
}

/*
 * Class:     unicorn_Unicorn
 * Method:    _emu_stop
 * Signature: (J)V
 */
JNIEXPORT void JNICALL Java_unicorn_Unicorn__1emu_1stop(JNIEnv *env,
                                                        jclass clazz, jlong uc)
{
    uc_err err = uc_emu_stop((uc_engine *)uc);
    if (err != UC_ERR_OK) {
        throwUnicornException(env, err);
        return;
    }
}

static uc_err generic_reg_read(jlong ptr, jint isContext, jint regid,
                               void *result)
{
    if (isContext) {
        return uc_context_reg_read((uc_context *)ptr, regid, result);
    } else {
        return uc_reg_read((uc_engine *)ptr, regid, result);
    }
}

static uc_err generic_reg_write(jlong ptr, jint isContext, jint regid,
                                const void *value)
{
    if (isContext) {
        return uc_context_reg_write((uc_context *)ptr, regid, value);
    } else {
        return uc_reg_write((uc_engine *)ptr, regid, value);
    }
}

/*
 * Class:     unicorn_Unicorn
 * Method:    _reg_read_long
 * Signature: (JII)J
 */
JNIEXPORT jlong JNICALL Java_unicorn_Unicorn__1reg_1read_1long(
    JNIEnv *env, jclass clazz, jlong ptr, jint isContext, jint regid)
{
    /* XXX: This is just *wrong* on big-endian hosts, since a register
    smaller than 8 bytes will be written into the MSBs. */
    uint64_t result = 0;
    uc_err err = generic_reg_read(ptr, isContext, regid, &result);
    if (err != UC_ERR_OK) {
        throwUnicornException(env, err);
        return 0;
    }
    return result;
}

/*
 * Class:     unicorn_Unicorn
 * Method:    _reg_read_bytes
 * Signature: (JII[B)V
 */
JNIEXPORT void JNICALL Java_unicorn_Unicorn__1reg_1read_1bytes(
    JNIEnv *env, jclass clazz, jlong ptr, jint isContext, jint regid,
    jbyteArray data)
{
    jbyte *arr = (*env)->GetByteArrayElements(env, data, NULL);
    uc_err err = generic_reg_read(ptr, isContext, regid, arr);
    (*env)->ReleaseByteArrayElements(env, data, arr, 0);
    if (err != UC_ERR_OK) {
        throwUnicornException(env, err);
        return;
    }
}

/*
 * Class:     unicorn_Unicorn
 * Method:    _reg_write_long
 * Signature: (JIIJ)V
 */
JNIEXPORT void JNICALL
Java_unicorn_Unicorn__1reg_1write_1long(JNIEnv *env, jclass clazz, jlong ptr,
                                        jint isContext, jint regid, jlong value)
{
    uint64_t cvalue = value;
    uc_err err = generic_reg_write(ptr, isContext, regid, &cvalue);
    if (err != UC_ERR_OK) {
        throwUnicornException(env, err);
        return;
    }
}

/*
 * Class:     unicorn_Unicorn
 * Method:    _reg_write_bytes
 * Signature: (JII[B)V
 */
JNIEXPORT void JNICALL Java_unicorn_Unicorn__1reg_1write_1bytes(
    JNIEnv *env, jclass clazz, jlong ptr, jint isContext, jint regid,
    jbyteArray data)
{
    jbyte *arr = (*env)->GetByteArrayElements(env, data, NULL);
    uc_err err = generic_reg_write(ptr, isContext, regid, arr);
    (*env)->ReleaseByteArrayElements(env, data, arr, JNI_ABORT);
    if (err != UC_ERR_OK) {
        throwUnicornException(env, err);
        return;
    }
}

/*
 * Class:     unicorn_Unicorn
 * Method:    _reg_read_x86_mmr
 * Signature: (JII)Lunicorn/X86_MMR;
 */
JNIEXPORT jobject JNICALL Java_unicorn_Unicorn__1reg_1read_1x86_1mmr(
    JNIEnv *env, jclass clazz, jlong ptr, jint isContext, jint regid)
{
    uc_x86_mmr reg = {0};
    uc_err err = generic_reg_read(ptr, isContext, regid, &reg);
    if (err != UC_ERR_OK) {
        throwUnicornException(env, err);
        return 0;
    }
    return makeX86_MMR(env, &reg);
}

/*
 * Class:     unicorn_Unicorn
 * Method:    _reg_write_x86_mmr
 * Signature: (JIISJII)V
 */
JNIEXPORT void JNICALL Java_unicorn_Unicorn__1reg_1write_1x86_1mmr(
    JNIEnv *env, jclass clazz, jlong ptr, jint isContext, jint regid,
    jshort selector, jlong base, jint limit, jint flags)
{
    uc_x86_mmr reg = {0};
    reg.selector = selector;
    reg.base = base;
    reg.limit = limit;
    reg.flags = flags;
    uc_err err = generic_reg_write(ptr, isContext, regid, &reg);
    if (err != UC_ERR_OK) {
        throwUnicornException(env, err);
        return;
    }
}

/*
 * Class:     unicorn_Unicorn
 * Method:    _reg_read_x86_msr
 * Signature: (JII)J
 */
JNIEXPORT jlong JNICALL Java_unicorn_Unicorn__1reg_1read_1x86_1msr(
    JNIEnv *env, jclass clazz, jlong ptr, jint isContext, jint rid)
{
    uc_x86_msr reg = {0};
    reg.rid = rid;
    uc_err err = generic_reg_read(ptr, isContext, UC_X86_REG_MSR, &reg);
    if (err != UC_ERR_OK) {
        throwUnicornException(env, err);
        return 0;
    }
    return reg.value;
}

/*
 * Class:     unicorn_Unicorn
 * Method:    _reg_write_x86_msr
 * Signature: (JIIJ)V
 */
JNIEXPORT void JNICALL Java_unicorn_Unicorn__1reg_1write_1x86_1msr(
    JNIEnv *env, jclass clazz, jlong ptr, jint isContext, jint rid, jlong value)
{
    uc_x86_msr reg = {0};
    reg.rid = rid;
    reg.value = value;
    uc_err err = generic_reg_write(ptr, isContext, UC_X86_REG_MSR, &reg);
    if (err != UC_ERR_OK) {
        throwUnicornException(env, err);
        return;
    }
}

/*
 * Class:     unicorn_Unicorn
 * Method:    _reg_read_arm_cp
 * Signature: (JIIIIIIII)J
 */
JNIEXPORT jlong JNICALL Java_unicorn_Unicorn__1reg_1read_1arm_1cp(
    JNIEnv *env, jclass clazz, jlong ptr, jint isContext, jint cp, jint is64,
    jint sec, jint crn, jint crm, jint opc1, jint opc2)
{
    uc_arm_cp_reg reg = {0};
    reg.cp = cp;
    reg.is64 = is64;
    reg.sec = sec;
    reg.crn = crn;
    reg.crm = crm;
    reg.opc1 = opc1;
    reg.opc2 = opc2;
    uc_err err = generic_reg_read(ptr, isContext, UC_ARM_REG_CP_REG, &reg);
    if (err != UC_ERR_OK) {
        throwUnicornException(env, err);
        return 0;
    }
    return reg.val;
}

/*
 * Class:     unicorn_Unicorn
 * Method:    _reg_write_arm_cp
 * Signature: (JIIIIIIIIJ)V
 */
JNIEXPORT void JNICALL Java_unicorn_Unicorn__1reg_1write_1arm_1cp(
    JNIEnv *env, jclass clazz, jlong ptr, jint isContext, jint cp, jint is64,
    jint sec, jint crn, jint crm, jint opc1, jint opc2, jlong value)
{
    uc_arm_cp_reg reg = {0};
    reg.cp = cp;
    reg.is64 = is64;
    reg.sec = sec;
    reg.crn = crn;
    reg.crm = crm;
    reg.opc1 = opc1;
    reg.opc2 = opc2;
    reg.val = value;
    uc_err err = generic_reg_write(ptr, isContext, UC_ARM_REG_CP_REG, &reg);
    if (err != UC_ERR_OK) {
        throwUnicornException(env, err);
        return;
    }
}

/*
 * Class:     unicorn_Unicorn
 * Method:    _reg_read_arm64_cp
 * Signature: (JIIIIII)J
 */
JNIEXPORT jlong JNICALL Java_unicorn_Unicorn__1reg_1read_1arm64_1cp(
    JNIEnv *env, jclass clazz, jlong ptr, jint isContext, jint crn, jint crm,
    jint op0, jint op1, jint op2)
{
    uc_arm64_cp_reg reg = {0};
    reg.crn = crn;
    reg.crm = crm;
    reg.op0 = op0;
    reg.op1 = op1;
    reg.op2 = op2;
    uc_err err = generic_reg_read(ptr, isContext, UC_ARM64_REG_CP_REG, &reg);
    if (err != UC_ERR_OK) {
        throwUnicornException(env, err);
        return 0;
    }
    return reg.val;
}

/*
 * Class:     unicorn_Unicorn
 * Method:    _reg_write_arm64_cp
 * Signature: (JIIIIIIJ)V
 */
JNIEXPORT void JNICALL Java_unicorn_Unicorn__1reg_1write_1arm64_1cp(
    JNIEnv *env, jclass clazz, jlong ptr, jint isContext, jint crn, jint crm,
    jint op0, jint op1, jint op2, jlong value)
{
    uc_arm64_cp_reg reg = {0};
    reg.crn = crn;
    reg.crm = crm;
    reg.op0 = op0;
    reg.op1 = op1;
    reg.op2 = op2;
    reg.val = value;
    uc_err err = generic_reg_write(ptr, isContext, UC_ARM64_REG_CP_REG, &reg);
    if (err != UC_ERR_OK) {
        throwUnicornException(env, err);
        return;
    }
}

/*
 * Class:     unicorn_Unicorn
 * Method:    _mem_read
 * Signature: (JJ[B)V
 */
JNIEXPORT void JNICALL Java_unicorn_Unicorn__1mem_1read(JNIEnv *env,
                                                        jclass clazz, jlong uc,
                                                        jlong address,
                                                        jbyteArray dest)
{
    jsize size = (*env)->GetArrayLength(env, dest);
    jbyte *arr = (*env)->GetByteArrayElements(env, dest, NULL);
    uc_err err = uc_mem_read((uc_engine *)uc, address, arr, size);
    (*env)->ReleaseByteArrayElements(env, dest, arr, 0);
    if (err != UC_ERR_OK) {
        throwUnicornException(env, err);
        return;
    }
}

/*
 * Class:     unicorn_Unicorn
 * Method:    _mem_write
 * Signature: (JJ[B)V
 */
JNIEXPORT void JNICALL Java_unicorn_Unicorn__1mem_1write(JNIEnv *env,
                                                         jclass clazz, jlong uc,
                                                         jlong address,
                                                         jbyteArray src)
{
    jsize size = (*env)->GetArrayLength(env, src);
    jbyte *arr = (*env)->GetByteArrayElements(env, src, NULL);
    uc_err err = uc_mem_write((uc_engine *)uc, address, arr, size);
    (*env)->ReleaseByteArrayElements(env, src, arr, JNI_ABORT);
    if (err != UC_ERR_OK) {
        throwUnicornException(env, err);
        return;
    }
}

/*
 * Class:     unicorn_Unicorn
 * Method:    _version
 * Signature: ()I
 */
JNIEXPORT jint JNICALL Java_unicorn_Unicorn__1version(JNIEnv *env, jclass clazz)
{
    return (jint)uc_version(NULL, NULL);
}

/*
 * Class:     unicorn_Unicorn
 * Method:    _arch_supported
 * Signature: (I)Z
 */
JNIEXPORT jboolean JNICALL Java_unicorn_Unicorn__1arch_1supported(JNIEnv *env,
                                                                  jclass clazz,
                                                                  jint arch)
{
    return (jboolean)(uc_arch_supported((uc_arch)arch) != 0);
}

/*
 * Class:     unicorn_Unicorn
 * Method:    _query
 * Signature: (JI)J
 */
JNIEXPORT jlong JNICALL Java_unicorn_Unicorn__1query(JNIEnv *env, jclass clazz,
                                                     jlong uc, jint type)
{
    size_t result;
    uc_err err = uc_query((uc_engine *)uc, type, &result);
    if (err != UC_ERR_OK) {
        throwUnicornException(env, err);
        return 0;
    }
    return result;
}

/*
 * Class:     unicorn_Unicorn
 * Method:    _errno
 * Signature: (J)I
 */
JNIEXPORT jint JNICALL Java_unicorn_Unicorn__1errno(JNIEnv *env, jclass clazz,
                                                    jlong uc)
{
    return uc_errno((uc_engine *)uc);
}

/*
 * Class:     unicorn_Unicorn
 * Method:    _strerror
 * Signature: (I)Ljava/lang/String;
 */
JNIEXPORT jstring JNICALL Java_unicorn_Unicorn__1strerror(JNIEnv *env,
                                                          jclass clazz,
                                                          jint code)
{
    const char *err = uc_strerror((int)code);
    return (*env)->NewStringUTF(env, err);
}

static void deleteHookWrapper(JNIEnv *env, struct hook_wrapper *hh)
{
    if (hh) {
        if (hh->unicorn)
            (*env)->DeleteGlobalRef(env, hh->unicorn);
        if (hh->hook_obj)
            (*env)->DeleteGlobalRef(env, hh->hook_obj);
        if (hh->user_data)
            (*env)->DeleteGlobalRef(env, hh->user_data);
        free(hh);
    }
}

static struct hook_wrapper *makeHookWrapper(JNIEnv *env, jobject self,
                                            jobject callback, jobject user_data,
                                            const char *hook_name,
                                            const char *hook_sig)
{
    struct hook_wrapper *hh = calloc(1, sizeof(struct hook_wrapper));
    if (!hh) {
        throwOutOfMemoryError(env, "Unable to allocate hook_wrapper");
        return NULL;
    }

    hh->unicorn = (*env)->NewGlobalRef(env, self);
    if (!hh->unicorn) {
        deleteHookWrapper(env, hh);
        return NULL;
    }

    hh->hook_obj = (*env)->NewGlobalRef(env, callback);
    if (!hh->hook_obj) {
        deleteHookWrapper(env, hh);
        return NULL;
    }

    jclass clazz = (*env)->GetObjectClass(env, callback);
    if (!clazz) {
        deleteHookWrapper(env, hh);
        return NULL;
    }

    hh->hook_meth = (*env)->GetMethodID(env, clazz, hook_name, hook_sig);
    if (!hh->hook_meth) {
        deleteHookWrapper(env, hh);
        return NULL;
    }

    if (user_data) {
        hh->user_data = (*env)->NewGlobalRef(env, user_data);
        if (!hh->user_data) {
            deleteHookWrapper(env, hh);
            return NULL;
        }
    }

    return hh;
}

/*
 * Class:     unicorn_Unicorn
 * Method:    _hook_add
 * Signature: (JILunicorn/Hook;Ljava/lang/Object;JJ)J
 */
JNIEXPORT jlong JNICALL
Java_unicorn_Unicorn__1hook_1add__JILunicorn_Hook_2Ljava_lang_Object_2JJ(
    JNIEnv *env, jobject self, jlong uc, jint type, jobject callback,
    jobject user_data, jlong begin, jlong end)
{
    const char *hook_sig;
    void *hook_callback;

    if (type == UC_HOOK_INTR) {
        hook_sig = sig_InterruptHook;
        hook_callback = cb_hookintr;
    } else if (type == UC_HOOK_CODE || type == UC_HOOK_BLOCK) {
        hook_sig = sig_CodeHook; // also BlockHook
        hook_callback = cb_hookcode;
    } else if ((type & UC_HOOK_MEM_INVALID) && !(type & ~UC_HOOK_MEM_INVALID)) {
        hook_sig = sig_EventMemHook;
        hook_callback = cb_eventmem;
    } else if ((type & UC_HOOK_MEM_VALID) && !(type & ~UC_HOOK_MEM_VALID)) {
        hook_sig = sig_MemHook;
        hook_callback = cb_hookmem;
    } else if (type == UC_HOOK_INSN_INVALID) {
        hook_sig = sig_InvalidInstructionHook;
        hook_callback = cb_hookinsn_invalid;
    } else if (type == UC_HOOK_EDGE_GENERATED) {
        hook_sig = sig_EdgeGeneratedHook;
        hook_callback = cb_edge_gen;
    } else if (type == UC_HOOK_TLB_FILL) {
        hook_sig = sig_TlbFillHook;
        hook_callback = cb_tlbevent;
    } else {
        throwUnicornException(env, UC_ERR_HOOK);
        return 0;
    }

    struct hook_wrapper *hh =
        makeHookWrapper(env, self, callback, user_data, "hook", hook_sig);
    uc_err err = uc_hook_add((uc_engine *)uc, &hh->uc_hh, type, hook_callback,
                             hh, begin, end);
    if (err != UC_ERR_OK) {
        throwUnicornException(env, err);
        deleteHookWrapper(env, hh);
        return 0;
    }
    return (jlong)hh;
}

/*
 * Class:     unicorn_Unicorn
 * Method:    _hook_add
 * Signature: (JILunicorn/Hook;Ljava/lang/Object;JJI)J
 */
JNIEXPORT jlong JNICALL
Java_unicorn_Unicorn__1hook_1add__JILunicorn_Hook_2Ljava_lang_Object_2JJI(
    JNIEnv *env, jobject self, jlong uc, jint type, jobject callback,
    jobject user_data, jlong begin, jlong end, jint arg)
{
    const char *hook_sig;
    void *hook_callback;

    if (type == UC_HOOK_INSN) {
        switch (arg) {
        case UC_X86_INS_IN:
            hook_sig = sig_InHook;
            hook_callback = cb_insn_in;
            break;
        case UC_X86_INS_OUT:
            hook_sig = sig_OutHook;
            hook_callback = cb_insn_out;
            break;
        case UC_X86_INS_SYSCALL:
        case UC_X86_INS_SYSENTER:
            hook_sig = sig_SyscallHook;
            hook_callback = cb_insn_syscall;
            break;
        case UC_X86_INS_CPUID:
            hook_sig = sig_CpuidHook;
            hook_callback = cb_insn_cpuid;
            break;
        case UC_ARM64_INS_MRS:
        case UC_ARM64_INS_MSR:
        case UC_ARM64_INS_SYS:
        case UC_ARM64_INS_SYSL:
            hook_sig = sig_Arm64SysHook;
            hook_callback = cb_insn_sys;
            break;
        default:
            throwUnicornException(env, UC_ERR_INSN_INVALID);
            return 0;
        }
    } else {
        throwUnicornException(env, UC_ERR_HOOK);
        return 0;
    }

    struct hook_wrapper *hh =
        makeHookWrapper(env, self, callback, user_data, "hook", hook_sig);
    uc_err err = uc_hook_add((uc_engine *)uc, &hh->uc_hh, type, hook_callback,
                             hh, begin, end, arg);
    if (err != UC_ERR_OK) {
        throwUnicornException(env, err);
        deleteHookWrapper(env, hh);
        return 0;
    }
    return (jlong)hh;
}

/*
 * Class:     unicorn_Unicorn
 * Method:    _hook_add
 * Signature: (JILunicorn/Hook;Ljava/lang/Object;JJII)J
 */
JNIEXPORT jlong JNICALL
Java_unicorn_Unicorn__1hook_1add__JILunicorn_Hook_2Ljava_lang_Object_2JJII(
    JNIEnv *env, jobject self, jlong uc, jint type, jobject callback,
    jobject user_data, jlong begin, jlong end, jint arg1, jint arg2)
{
    const char *hook_sig;
    void *hook_callback;

    if (type == UC_HOOK_TCG_OPCODE) {
        hook_sig = sig_TcgOpcodeHook;
        hook_callback = cb_tcg_op_2;
    } else {
        throwUnicornException(env, UC_ERR_HOOK);
        return 0;
    }

    struct hook_wrapper *hh =
        makeHookWrapper(env, self, callback, user_data, "hook", hook_sig);
    uc_err err = uc_hook_add((uc_engine *)uc, &hh->uc_hh, type, hook_callback,
                             hh, begin, end, arg1, arg2);
    if (err != UC_ERR_OK) {
        throwUnicornException(env, err);
        deleteHookWrapper(env, hh);
        return 0;
    }
    return (jlong)hh;
}

/*
 * Class:     unicorn_Unicorn
 * Method:    _hook_del
 * Signature: (JJ)V
 */
JNIEXPORT void JNICALL Java_unicorn_Unicorn__1hook_1del(JNIEnv *env,
                                                        jclass clazz, jlong uc,
                                                        jlong hh)
{
    struct hook_wrapper *h = (struct hook_wrapper *)hh;
    uc_hook_del((uc_engine *)uc, h->uc_hh);
    if (h->unicorn) {
        (*env)->DeleteGlobalRef(env, h->unicorn);
        h->unicorn = NULL;
    }
    if (h->hook_obj) {
        (*env)->DeleteGlobalRef(env, h->hook_obj);
        h->hook_obj = NULL;
    }
    if (h->user_data) {
        (*env)->DeleteGlobalRef(env, h->user_data);
        h->user_data = NULL;
    }
}

/*
 * Class:     unicorn_Unicorn
 * Method:    _hookwrapper_free
 * Signature: (J)V
 */
JNIEXPORT void JNICALL Java_unicorn_Unicorn__1hookwrapper_1free(JNIEnv *env,
                                                                jclass clazz,
                                                                jlong hh)
{
    deleteHookWrapper(env, (struct hook_wrapper *)hh);
}

/*
 * Class:     unicorn_Unicorn
 * Method:    _mmio_map
 * Signature:
 * (JJJLunicorn/MmioReadHandler;Ljava/lang/Object;Lunicorn/MmioWriteHandler;Ljava/lang/Object;)[J
 */
JNIEXPORT jlongArray JNICALL Java_unicorn_Unicorn__1mmio_1map(
    JNIEnv *env, jobject self, jlong uc, jlong address, jlong size,
    jobject read_cb, jobject user_data_read, jobject write_cb,
    jobject user_data_write)
{
    struct hook_wrapper *hooks[2] = {0};

    if (read_cb) {
        hooks[0] = makeHookWrapper(env, self, read_cb, user_data_read, "read",
                                   sig_MmioReadHandler);
        if (!hooks[0]) {
            goto fail;
        }
    }

    if (write_cb) {
        hooks[1] = makeHookWrapper(env, self, write_cb, user_data_write,
                                   "write", sig_MmioWriteHandler);
        if (!hooks[1]) {
            goto fail;
        }
    }

    jlong hooksLong[2];
    size_t hooksCount = 0;
    if (hooks[0])
        hooksLong[hooksCount++] = (jlong)hooks[0];
    if (hooks[1])
        hooksLong[hooksCount++] = (jlong)hooks[1];

    jlongArray result = (*env)->NewLongArray(env, hooksCount);
    if (result == NULL) {
        goto fail;
    }
    (*env)->SetLongArrayRegion(env, result, 0, hooksCount, hooksLong);

    uc_err err = uc_mmio_map((uc_engine *)uc, address, size,
                             (hooks[0] ? cb_mmio_read : NULL), hooks[0],
                             (hooks[1] ? cb_mmio_write : NULL), hooks[1]);
    if (err != UC_ERR_OK) {
        throwUnicornException(env, err);
        goto fail;
    }
    return result;
fail:
    deleteHookWrapper(env, hooks[0]);
    deleteHookWrapper(env, hooks[1]);
    return NULL;
}

/*
 * Class:     unicorn_Unicorn
 * Method:    _mem_map
 * Signature: (JJJI)V
 */
JNIEXPORT void JNICALL Java_unicorn_Unicorn__1mem_1map(JNIEnv *env,
                                                       jclass clazz, jlong uc,
                                                       jlong address,
                                                       jlong size, jint perms)
{
    uc_err err = uc_mem_map((uc_engine *)uc, address, size, perms);
    if (err != UC_ERR_OK) {
        throwUnicornException(env, err);
        return;
    }
}

/*
 * Class:     unicorn_Unicorn
 * Method:    _mem_map_ptr
 * Signature: (JJLjava/nio/Buffer;I)V
 */
JNIEXPORT void JNICALL Java_unicorn_Unicorn__1mem_1map_1ptr(
    JNIEnv *env, jclass clazz, jlong uc, jlong address, jobject buf, jint perms)
{
    jlong size = (*env)->GetDirectBufferCapacity(env, buf);
    void *host_address = (*env)->GetDirectBufferAddress(env, buf);
    if (size < 0 || host_address == NULL) {
        throwCustomUnicornException(env,
                                    "mem_map_ptr requires a direct buffer");
        return;
    }

    uc_err err =
        uc_mem_map_ptr((uc_engine *)uc, address, size, perms, host_address);
    if (err != UC_ERR_OK) {
        throwUnicornException(env, err);
        return;
    }
}

/*
 * Class:     unicorn_Unicorn
 * Method:    _mem_unmap
 * Signature: (JJJ)V
 */
JNIEXPORT void JNICALL Java_unicorn_Unicorn__1mem_1unmap(JNIEnv *env,
                                                         jclass clazz, jlong uc,
                                                         jlong address,
                                                         jlong size)
{
    uc_err err = uc_mem_unmap((uc_engine *)uc, address, size);
    if (err != UC_ERR_OK) {
        throwUnicornException(env, err);
        return;
    }
}

/*
 * Class:     unicorn_Unicorn
 * Method:    _mem_protect
 * Signature: (JJJI)V
 */
JNIEXPORT void JNICALL Java_unicorn_Unicorn__1mem_1protect(
    JNIEnv *env, jclass clazz, jlong uc, jlong address, jlong size, jint perms)
{
    uc_err err = uc_mem_protect((uc_engine *)uc, address, size, perms);
    if (err != UC_ERR_OK) {
        throwUnicornException(env, err);
        return;
    }
}

/*
 * Class:     unicorn_Unicorn
 * Method:    _mem_regions
 * Signature: (J)[Lunicorn/MemRegion;
 */
JNIEXPORT jobjectArray JNICALL
Java_unicorn_Unicorn__1mem_1regions(JNIEnv *env, jclass uc_clazz, jlong uc)
{
    static jclass clazz;
    if (!clazz) {
        clazz = (*env)->FindClass(env, "unicorn/MemRegion");
        if (!clazz)
            return NULL;
        clazz = (*env)->NewGlobalRef(env, clazz);
        if (!clazz)
            return NULL;
    }

    static jmethodID clazzInit;
    if (!clazzInit) {
        clazzInit = (*env)->GetMethodID(env, clazz, "<init>", "(JJI)V");
        if (!clazzInit)
            return NULL;
    }

    uc_mem_region *regions = NULL;
    uint32_t count = 0;
    uint32_t i;

    uc_err err = uc_mem_regions((uc_engine *)uc, &regions, &count);
    if (err != UC_ERR_OK) {
        throwUnicornException(env, err);
        return NULL;
    }

    jobjectArray result =
        (*env)->NewObjectArray(env, (jsize)count, clazz, NULL);
    if (!result) {
        uc_free(regions);
        return NULL;
    }

    for (i = 0; i < count; i++) {
        jobject mr =
            (*env)->NewObject(env, clazz, clazzInit, (jlong)regions[i].begin,
                              (jlong)regions[i].end, (jint)regions[i].perms);
        if (!mr) {
            uc_free(regions);
            return NULL;
        }
        (*env)->SetObjectArrayElement(env, result, (jsize)i, mr);
    }
    uc_free(regions);

    return result;
}

/*
 * Class:     unicorn_Unicorn
 * Method:    _context_alloc
 * Signature: (J)J
 */
JNIEXPORT jlong JNICALL Java_unicorn_Unicorn__1context_1alloc(JNIEnv *env,
                                                              jclass clazz,
                                                              jlong uc)
{
    uc_context *ctx;
    uc_err err = uc_context_alloc((uc_engine *)uc, &ctx);
    if (err != UC_ERR_OK) {
        throwUnicornException(env, err);
        return 0;
    }
    return (jlong)ctx;
}

/*
 * Class:     unicorn_Unicorn
 * Method:    _context_free
 * Signature: (J)V
 */
JNIEXPORT void JNICALL Java_unicorn_Unicorn__1context_1free(JNIEnv *env,
                                                            jclass clazz,
                                                            jlong ctx)
{
    uc_err err = uc_context_free((uc_context *)ctx);
    if (err != UC_ERR_OK) {
        throwUnicornException(env, err);
        return;
    }
}

/*
 * Class:     unicorn_Unicorn
 * Method:    _context_save
 * Signature: (JJ)V
 */
JNIEXPORT void JNICALL Java_unicorn_Unicorn__1context_1save(JNIEnv *env,
                                                            jclass clazz,
                                                            jlong uc, jlong ctx)
{
    uc_err err = uc_context_save((uc_engine *)uc, (uc_context *)ctx);
    if (err != UC_ERR_OK) {
        throwUnicornException(env, err);
        return;
    }
}

/*
 * Class:     unicorn_Unicorn
 * Method:    _context_restore
 * Signature: (JJ)V
 */
JNIEXPORT void JNICALL Java_unicorn_Unicorn__1context_1restore(JNIEnv *env,
                                                               jclass clazz,
                                                               jlong uc,
                                                               jlong ctx)
{
    uc_err err = uc_context_restore((uc_engine *)uc, (uc_context *)ctx);
    if (err != UC_ERR_OK) {
        throwUnicornException(env, err);
        return;
    }
}

/*
 * Class:     unicorn_Unicorn
 * Method:    _ctl_get_mode
 * Signature: (J)I
 */
JNIEXPORT jint JNICALL Java_unicorn_Unicorn__1ctl_1get_1mode(JNIEnv *env,
                                                             jclass clazz,
                                                             jlong uc)
{
    int mode;
    uc_err err = uc_ctl_get_mode((uc_engine *)uc, &mode);
    if (err != UC_ERR_OK) {
        throwUnicornException(env, err);
        return 0;
    }
    return mode;
}

/*
 * Class:     unicorn_Unicorn
 * Method:    _ctl_get_arch
 * Signature: (J)I
 */
JNIEXPORT jint JNICALL Java_unicorn_Unicorn__1ctl_1get_1arch(JNIEnv *env,
                                                             jclass clazz,
                                                             jlong uc)
{
    int arch;
    uc_err err = uc_ctl_get_arch((uc_engine *)uc, &arch);
    if (err != UC_ERR_OK) {
        throwUnicornException(env, err);
        return 0;
    }
    return arch;
}

/*
 * Class:     unicorn_Unicorn
 * Method:    _ctl_get_timeout
 * Signature: (J)J
 */
JNIEXPORT jlong JNICALL Java_unicorn_Unicorn__1ctl_1get_1timeout(JNIEnv *env,
                                                                 jclass clazz,
                                                                 jlong uc)
{
    uint64_t timeout;
    uc_err err = uc_ctl_get_timeout((uc_engine *)uc, &timeout);
    if (err != UC_ERR_OK) {
        throwUnicornException(env, err);
        return 0;
    }
    return timeout;
}

/*
 * Class:     unicorn_Unicorn
 * Method:    _ctl_get_page_size
 * Signature: (J)I
 */
JNIEXPORT jint JNICALL Java_unicorn_Unicorn__1ctl_1get_1page_1size(JNIEnv *env,
                                                                   jclass clazz,
                                                                   jlong uc)
{
    uint32_t page_size;
    uc_err err = uc_ctl_get_page_size((uc_engine *)uc, &page_size);
    if (err != UC_ERR_OK) {
        throwUnicornException(env, err);
        return 0;
    }
    return page_size;
}

/*
 * Class:     unicorn_Unicorn
 * Method:    _ctl_set_page_size
 * Signature: (JI)V
 */
JNIEXPORT void JNICALL Java_unicorn_Unicorn__1ctl_1set_1page_1size(
    JNIEnv *env, jclass clazz, jlong uc, jint page_size)
{
    uc_err err = uc_ctl_set_page_size((uc_engine *)uc, (uint32_t)page_size);
    if (err != UC_ERR_OK) {
        throwUnicornException(env, err);
        return;
    }
}

/*
 * Class:     unicorn_Unicorn
 * Method:    _ctl_set_use_exits
 * Signature: (JZ)V
 */
JNIEXPORT void JNICALL Java_unicorn_Unicorn__1ctl_1set_1use_1exits(
    JNIEnv *env, jclass clazz, jlong uc, jboolean value)
{
    uc_err err;
    if (value) {
        err = uc_ctl_exits_enable((uc_engine *)uc);
    } else {
        err = uc_ctl_exits_disable((uc_engine *)uc);
    }
    if (err != UC_ERR_OK) {
        throwUnicornException(env, err);
        return;
    }
}

/*
 * Class:     unicorn_Unicorn
 * Method:    _ctl_get_exits_cnt
 * Signature: (J)J
 */
JNIEXPORT jlong JNICALL
Java_unicorn_Unicorn__1ctl_1get_1exits_1cnt(JNIEnv *env, jclass clazz, jlong uc)
{
    size_t exits_cnt;
    uc_err err = uc_ctl_get_exits_cnt((uc_engine *)uc, &exits_cnt);
    if (err != UC_ERR_OK) {
        throwUnicornException(env, err);
        return 0;
    }
    return exits_cnt;
}

/*
 * Class:     unicorn_Unicorn
 * Method:    _ctl_get_exits
 * Signature: (J)[J
 */
JNIEXPORT jlongArray JNICALL
Java_unicorn_Unicorn__1ctl_1get_1exits(JNIEnv *env, jclass clazz, jlong uc)
{
    size_t exits_cnt;
    uc_err err = uc_ctl_get_exits_cnt((uc_engine *)uc, &exits_cnt);
    if (err != UC_ERR_OK) {
        throwUnicornException(env, err);
        return 0;
    }

    jlongArray result = (*env)->NewLongArray(env, (jsize)exits_cnt);
    if (!result)
        return NULL;

    jlong *resultArr = (*env)->GetLongArrayElements(env, result, NULL);
    if (!resultArr)
        return NULL;

    err = uc_ctl_get_exits((uc_engine *)uc, (uint64_t *)resultArr, exits_cnt);
    (*env)->ReleaseLongArrayElements(env, result, resultArr, 0);
    if (err != UC_ERR_OK) {
        throwUnicornException(env, err);
        return 0;
    }
    return result;
}

/*
 * Class:     unicorn_Unicorn
 * Method:    _ctl_set_exits
 * Signature: (J[J)V
 */
JNIEXPORT void JNICALL Java_unicorn_Unicorn__1ctl_1set_1exits(JNIEnv *env,
                                                              jclass clazz,
                                                              jlong uc,
                                                              jlongArray exits)
{
    jsize count = (*env)->GetArrayLength(env, exits);
    jlong *arr = (*env)->GetLongArrayElements(env, exits, NULL);
    if (!arr)
        return;

    uc_err err =
        uc_ctl_set_exits((uc_engine *)uc, (uint64_t *)arr, (size_t)count);
    (*env)->ReleaseLongArrayElements(env, exits, arr, JNI_ABORT);
    if (err != UC_ERR_OK) {
        throwUnicornException(env, err);
        return;
    }
}

/*
 * Class:     unicorn_Unicorn
 * Method:    _ctl_get_cpu_model
 * Signature: (J)I
 */
JNIEXPORT jint JNICALL Java_unicorn_Unicorn__1ctl_1get_1cpu_1model(JNIEnv *env,
                                                                   jclass clazz,
                                                                   jlong uc)
{
    int cpu_model;
    uc_err err = uc_ctl_get_cpu_model((uc_engine *)uc, &cpu_model);
    if (err != UC_ERR_OK) {
        throwUnicornException(env, err);
        return 0;
    }
    return cpu_model;
}

/*
 * Class:     unicorn_Unicorn
 * Method:    _ctl_set_cpu_model
 * Signature: (JI)V
 */
JNIEXPORT void JNICALL Java_unicorn_Unicorn__1ctl_1set_1cpu_1model(
    JNIEnv *env, jclass clazz, jlong uc, jint cpu_model)
{
    uc_err err = uc_ctl_set_cpu_model((uc_engine *)uc, (int)cpu_model);
    if (err != UC_ERR_OK) {
        throwUnicornException(env, err);
        return;
    }
}

/*
 * Class:     unicorn_Unicorn
 * Method:    _ctl_request_cache
 * Signature: (JJ)Lunicorn/TranslationBlock;
 */
JNIEXPORT jobject JNICALL Java_unicorn_Unicorn__1ctl_1request_1cache(
    JNIEnv *env, jclass clazz, jlong uc, jlong address)
{
    uc_tb tb;
    uc_err err = uc_ctl_request_cache((uc_engine *)uc, (uint64_t)address, &tb);
    if (err != UC_ERR_OK) {
        throwUnicornException(env, err);
        return NULL;
    }
    return makeTranslationBlock(env, &tb);
}

/*
 * Class:     unicorn_Unicorn
 * Method:    _ctl_remove_cache
 * Signature: (JJJ)V
 */
JNIEXPORT void JNICALL Java_unicorn_Unicorn__1ctl_1remove_1cache(
    JNIEnv *env, jclass clazz, jlong uc, jlong address, jlong end)
{
    uc_err err =
        uc_ctl_remove_cache((uc_engine *)uc, (uint64_t)address, (uint64_t)end);
    if (err != UC_ERR_OK) {
        throwUnicornException(env, err);
        return;
    }
}

/*
 * Class:     unicorn_Unicorn
 * Method:    _ctl_flush_tb
 * Signature: (J)V
 */
JNIEXPORT void JNICALL Java_unicorn_Unicorn__1ctl_1flush_1tb(JNIEnv *env,
                                                             jclass clazz,
                                                             jlong uc)
{
    uc_err err = uc_ctl_flush_tb((uc_engine *)uc);
    if (err != UC_ERR_OK) {
        throwUnicornException(env, err);
        return;
    }
}

/*
 * Class:     unicorn_Unicorn
 * Method:    _ctl_flush_tlb
 * Signature: (J)V
 */
JNIEXPORT void JNICALL Java_unicorn_Unicorn__1ctl_1flush_1tlb(JNIEnv *env,
                                                              jclass clazz,
                                                              jlong uc)
{
    uc_err err = uc_ctl_flush_tlb((uc_engine *)uc);
    if (err != UC_ERR_OK) {
        throwUnicornException(env, err);
        return;
    }
}

/*
 * Class:     unicorn_Unicorn
 * Method:    _ctl_tlb_mode
 * Signature: (JI)V
 */
JNIEXPORT void JNICALL Java_unicorn_Unicorn__1ctl_1tlb_1mode(JNIEnv *env,
                                                             jclass clazz,
                                                             jlong uc,
                                                             jint mode)
{
    uc_err err = uc_ctl_tlb_mode((uc_engine *)uc, (int)mode);
    if (err != UC_ERR_OK) {
        throwUnicornException(env, err);
        return;
    }
}
