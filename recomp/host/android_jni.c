/* android_jni.c: the first launch on Android. The Java setup screen (SetupActivity) has copied the player's DLC
 * Edition exe out of the file they picked; unpack() runs evb_unpack on it (on the activity's worker thread), its
 * progress going back to the activity's onUnpackProgress(String, int). Or they picked the DLC's patch and then their
 * PvZ exe: unpackPatched() makes the DLC Edition exe from them and unpacks it (dlc_setup.c). Each returns null when
 * done, else the message. */
#include <jni.h>

#include "dlc_setup.h"

static JNIEnv *cb_env;
static jobject cb_self;
static jmethodID cb_progress;

static void progress(const char *what, int permille)
{
    jstring s = (*cb_env)->NewStringUTF(cb_env, what);
    (*cb_env)->CallVoidMethod(cb_env, cb_self, cb_progress, s, (jint)permille);
    (*cb_env)->DeleteLocalRef(cb_env, s);
}

JNIEXPORT jstring JNICALL Java_io_github_aks796_pvzdlc_SetupActivity_unpack(JNIEnv *env, jobject self, jstring exe, jstring dir)
{
    const char *e = (*env)->GetStringUTFChars(env, exe, NULL), *d = (*env)->GetStringUTFChars(env, dir, NULL);
    cb_env = env, cb_self = self;
    cb_progress = (*env)->GetMethodID(env, (*env)->GetObjectClass(env, self), "onUnpackProgress", "(Ljava/lang/String;I)V");
    char err[400];
    int r = evb_unpack(e, d, cb_progress ? progress : NULL, err, sizeof err);
    (*env)->ReleaseStringUTFChars(env, exe, e);
    (*env)->ReleaseStringUTFChars(env, dir, d);
    return r ? (*env)->NewStringUTF(env, err) : NULL;
}

JNIEXPORT jstring JNICALL Java_io_github_aks796_pvzdlc_SetupActivity_unpackPatched(JNIEnv *env, jobject self, jstring patch,
                                                                                  jstring base, jstring dir)
{
    const char *p = (*env)->GetStringUTFChars(env, patch, NULL), *b = (*env)->GetStringUTFChars(env, base, NULL),
               *d = (*env)->GetStringUTFChars(env, dir, NULL);
    cb_env = env, cb_self = self;
    cb_progress = (*env)->GetMethodID(env, (*env)->GetObjectClass(env, self), "onUnpackProgress", "(Ljava/lang/String;I)V");
    char err[400];
    int r = dlc_setup_from_files(p, b, d, cb_progress ? progress : NULL, err, sizeof err);
    (*env)->ReleaseStringUTFChars(env, patch, p);
    (*env)->ReleaseStringUTFChars(env, base, b);
    (*env)->ReleaseStringUTFChars(env, dir, d);
    return r ? (*env)->NewStringUTF(env, err) : NULL;
}
