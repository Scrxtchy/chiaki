/*
 * This file is part of Chiaki.
 *
 * Chiaki is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * Chiaki is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with Chiaki.  If not, see <https://www.gnu.org/licenses/>.
 */

#include <jni.h>

#include <android/log.h>

#include <chiaki/common.h>
#include <chiaki/log.h>
#include <chiaki/session.h>
#include <chiaki/discoveryservice.h>
#include <chiaki/regist.h>

#include <string.h>
#include <linux/in.h>
#include <linux/in6.h>
#include <arpa/inet.h>

#include "video-decoder.h"
#include "audio-decoder.h"
#include "audio-output.h"

#define LOG_TAG "Chiaki"
#define JNI_VERSION JNI_VERSION_1_6

#define BASE_PACKAGE "com/metallic/chiaki/lib"
#define JNI_FCN(name) Java_com_metallic_chiaki_lib_ChiakiNative_##name

#define E (*env)

static void log_cb_android(ChiakiLogLevel level, const char *msg, void *user)
{
	int prio;
	switch(level)
	{
		case CHIAKI_LOG_DEBUG:
			prio = ANDROID_LOG_DEBUG;
			break;
		case CHIAKI_LOG_VERBOSE:
			prio = ANDROID_LOG_VERBOSE;
			break;
		case CHIAKI_LOG_INFO:
			prio = ANDROID_LOG_INFO;
			break;
		case CHIAKI_LOG_WARNING:
			prio = ANDROID_LOG_ERROR;
			break;
		case CHIAKI_LOG_ERROR:
			prio = ANDROID_LOG_ERROR;
			break;
		default:
			prio = ANDROID_LOG_INFO;
			break;
	}
	__android_log_write(prio, LOG_TAG, msg);
}

static char *strdup_jni(const char *str)
{
	if(!str)
		return NULL;
	char *r = strdup(str);
	if(!r)
		return NULL;
	for(char *c=r; *c; c++)
	{
		if(*c & (1 << 7))
			*c = '?';
	}
	return r;
}

static jobject jnistr_from_ascii(JNIEnv *env, const char *str)
{
	if(!str)
		return NULL;
	char *s = strdup_jni(str);
	if(!s)
		return NULL;
	jobject r = E->NewStringUTF(env, s);
	free(s);
	return r;
}

static ChiakiLog global_log;
static JavaVM *global_vm;

JNIEXPORT jint JNI_OnLoad(JavaVM *vm, void *reserved)
{
	global_vm = vm;
	chiaki_log_init(&global_log, CHIAKI_LOG_ALL & ~CHIAKI_LOG_VERBOSE, log_cb_android, NULL);
	CHIAKI_LOGI(&global_log, "Loading Chiaki Library");
	ChiakiErrorCode err = chiaki_lib_init();
	CHIAKI_LOGI(&global_log, "Chiaki Library Init Result: %s\n", chiaki_error_string(err));
	return JNI_VERSION;
}

JNIEXPORT jstring JNICALL JNI_FCN(errorCodeToString)(JNIEnv *env, jobject obj, jint value)
{
	return E->NewStringUTF(env, chiaki_error_string((ChiakiErrorCode)value));
}

JNIEXPORT jstring JNICALL JNI_FCN(quitReasonToString)(JNIEnv *env, jobject obj, jint value)
{
	return E->NewStringUTF(env, chiaki_quit_reason_string((ChiakiQuitReason)value));
}

JNIEXPORT jboolean JNICALL JNI_FCN(quitReasonIsStopped)(JNIEnv *env, jobject obj, jint value)
{
	return value == CHIAKI_QUIT_REASON_STOPPED;
}

typedef struct android_chiaki_session_t
{
	ChiakiSession session;
	jobject java_session;
	jclass java_session_class;
	jmethodID java_session_event_connected_meth;
	jmethodID java_session_event_login_pin_request_meth;
	jmethodID java_session_event_quit_meth;
	jfieldID java_controller_state_buttons;
	jfieldID java_controller_state_l2_state;
	jfieldID java_controller_state_r2_state;
	jfieldID java_controller_state_left_x;
	jfieldID java_controller_state_left_y;
	jfieldID java_controller_state_right_x;
	jfieldID java_controller_state_right_y;

	AndroidChiakiVideoDecoder video_decoder;
	AndroidChiakiAudioDecoder audio_decoder;
	void *audio_output;
} AndroidChiakiSession;

static JNIEnv *attach_thread_jni()
{
	JNIEnv *env;
	int r = (*global_vm)->GetEnv(global_vm, (void **)&env, JNI_VERSION);
	if(r == JNI_OK)
		return env;

	if((*global_vm)->AttachCurrentThread(global_vm, &env, NULL) == 0)
		return env;

	CHIAKI_LOGE(&global_log, "Failed to get JNIEnv from JavaVM or attach");
	return NULL;
}

typedef struct android_chiaki_log_t
{
	jobject java_log;
	jmethodID java_log_meth;
	ChiakiLog log;
} AndroidChiakiLog;

static void android_chiaki_log_cb(ChiakiLogLevel level, const char *msg, void *user)
{
	log_cb_android(level, msg, NULL);

	AndroidChiakiLog *log = user;
	JNIEnv *env = attach_thread_jni();
	if(!env)
		return;
	E->CallVoidMethod(env, log->java_log, log->java_log_meth, (jint)level, jnistr_from_ascii(env, msg));
	(*global_vm)->DetachCurrentThread(global_vm);
}

static void android_chiaki_log_init(AndroidChiakiLog *log, JNIEnv *env, jobject java_log)
{
	log->java_log = E->NewGlobalRef(env, java_log);
	jclass log_class = E->GetObjectClass(env, log->java_log);
	log->java_log_meth = E->GetMethodID(env, log_class, "log", "(ILjava/lang/String;)V");
	log->log.level_mask = (uint32_t)E->GetIntField(env, log->java_log, E->GetFieldID(env, log_class, "levelMask", "I"));
	log->log.cb = android_chiaki_log_cb;
	log->log.user = log;
}

static void android_chiaki_log_fini(AndroidChiakiLog *log, JNIEnv *env)
{
	E->DeleteGlobalRef(env, log->java_log);
}

static void android_chiaki_event_cb(ChiakiEvent *event, void *user)
{
	AndroidChiakiSession *session = user;

	JNIEnv *env = attach_thread_jni();
	if(!env)
		return;

	switch(event->type)
	{
		case CHIAKI_EVENT_CONNECTED:
			E->CallVoidMethod(env, session->java_session,
							  session->java_session_event_connected_meth);
			break;
		case CHIAKI_EVENT_LOGIN_PIN_REQUEST:
			E->CallVoidMethod(env, session->java_session,
							  session->java_session_event_login_pin_request_meth,
							  (jboolean)event->login_pin_request.pin_incorrect);
			break;
		case CHIAKI_EVENT_QUIT:
		{
			char *reason_str = strdup_jni(event->quit.reason_str);
			jstring reason_str_java = reason_str ? E->NewStringUTF(env, reason_str) : NULL;
			E->CallVoidMethod(env, session->java_session,
							  session->java_session_event_quit_meth,
							  (jint)event->quit.reason,
							  reason_str_java);
			if(reason_str_java)
				E->DeleteLocalRef(env, reason_str_java);
			free(reason_str);
			break;
		}
	}

	(*global_vm)->DetachCurrentThread(global_vm);
}

JNIEXPORT void JNICALL JNI_FCN(sessionCreate)(JNIEnv *env, jobject obj, jobject result, jobject connect_info_obj, jobject java_session)
{
	AndroidChiakiSession *session = NULL;
	ChiakiErrorCode err = CHIAKI_ERR_SUCCESS;
	char *host_str = NULL;

	jclass result_class = E->GetObjectClass(env, result);

	jclass connect_info_class = E->GetObjectClass(env, connect_info_obj);
	jstring host_string = E->GetObjectField(env, connect_info_obj, E->GetFieldID(env, connect_info_class, "host", "Ljava/lang/String;"));
	jbyteArray regist_key_array = E->GetObjectField(env, connect_info_obj, E->GetFieldID(env, connect_info_class, "registKey", "[B"));
	jbyteArray morning_array = E->GetObjectField(env, connect_info_obj, E->GetFieldID(env, connect_info_class, "morning", "[B"));
	jobject connect_video_profile_obj = E->GetObjectField(env, connect_info_obj, E->GetFieldID(env, connect_info_class, "videoProfile", "L"BASE_PACKAGE"/ConnectVideoProfile;"));
	jclass connect_video_profile_class = E->GetObjectClass(env, connect_video_profile_obj);

	ChiakiConnectInfo connect_info;
	const char *str_borrow = E->GetStringUTFChars(env, host_string, NULL);
	connect_info.host = host_str = strdup(str_borrow);
	E->ReleaseStringUTFChars(env, host_string, str_borrow);
	if(!connect_info.host)
	{
		err = CHIAKI_ERR_MEMORY;
		goto beach;
	}

	if(E->GetArrayLength(env, regist_key_array) != sizeof(connect_info.regist_key))
	{
		CHIAKI_LOGE(&global_log, "Regist Key passed from Java has invalid length");
		err = CHIAKI_ERR_INVALID_DATA;
		goto beach;
	}
	jbyte *bytes = E->GetByteArrayElements(env, regist_key_array, NULL);
	memcpy(connect_info.regist_key, bytes, sizeof(connect_info.regist_key));
	E->ReleaseByteArrayElements(env, regist_key_array, bytes, JNI_ABORT);

	if(E->GetArrayLength(env, morning_array) != sizeof(connect_info.morning))
	{
		CHIAKI_LOGE(&global_log, "Morning passed from Java has invalid length");
		err = CHIAKI_ERR_INVALID_DATA;
		goto beach;
	}
	bytes = E->GetByteArrayElements(env, morning_array, NULL);
	memcpy(connect_info.morning, bytes, sizeof(connect_info.morning));
	E->ReleaseByteArrayElements(env, morning_array, bytes, JNI_ABORT);

	connect_info.video_profile.width = (unsigned int)E->GetIntField(env, connect_video_profile_obj, E->GetFieldID(env, connect_video_profile_class, "width", "I"));
	connect_info.video_profile.height = (unsigned int)E->GetIntField(env, connect_video_profile_obj, E->GetFieldID(env, connect_video_profile_class, "height", "I"));
	connect_info.video_profile.max_fps = (unsigned int)E->GetIntField(env, connect_video_profile_obj, E->GetFieldID(env, connect_video_profile_class, "maxFPS", "I"));
	connect_info.video_profile.bitrate = (unsigned int)E->GetIntField(env, connect_video_profile_obj, E->GetFieldID(env, connect_video_profile_class, "bitrate", "I"));

	session = CHIAKI_NEW(AndroidChiakiSession);
	if(!session)
	{
		err = CHIAKI_ERR_MEMORY;
		goto beach;
	}
	memset(session, 0, sizeof(AndroidChiakiSession));
	err = android_chiaki_video_decoder_init(&session->video_decoder, &global_log);
	if(err != CHIAKI_ERR_SUCCESS)
	{
		free(session);
		session = NULL;
		goto beach;
	}

	err = android_chiaki_audio_decoder_init(&session->audio_decoder, &global_log);
	if(err != CHIAKI_ERR_SUCCESS)
	{
		android_chiaki_video_decoder_fini(&session->video_decoder);
		free(session);
		session = NULL;
		goto beach;
	}

	session->audio_output = android_chiaki_audio_output_new(&global_log);

	android_chiaki_audio_decoder_set_cb(&session->audio_decoder, android_chiaki_audio_output_settings, android_chiaki_audio_output_frame, session->audio_output);

	err = chiaki_session_init(&session->session, &connect_info, &global_log);
	if(err != CHIAKI_ERR_SUCCESS)
	{
		CHIAKI_LOGE(&global_log, "JNI ChiakiSession failed to init");
		android_chiaki_video_decoder_fini(&session->video_decoder);
		android_chiaki_audio_decoder_fini(&session->audio_decoder);
		android_chiaki_audio_output_free(session->audio_output);
		free(session);
		session = NULL;
		goto beach;
	}

	session->java_session = E->NewGlobalRef(env, java_session);
	session->java_session_class = E->NewGlobalRef(env, E->GetObjectClass(env, session->java_session));
	session->java_session_event_connected_meth = E->GetMethodID(env, session->java_session_class, "eventConnected", "()V");
	session->java_session_event_login_pin_request_meth = E->GetMethodID(env, session->java_session_class, "eventLoginPinRequest", "(Z)V");
	session->java_session_event_quit_meth = E->GetMethodID(env, session->java_session_class, "eventQuit", "(ILjava/lang/String;)V");

	jclass controller_state_class = E->FindClass(env, BASE_PACKAGE"/ControllerState");
	session->java_controller_state_buttons = E->GetFieldID(env, controller_state_class, "buttons", "I");
	session->java_controller_state_l2_state = E->GetFieldID(env, controller_state_class, "l2State", "B");
	session->java_controller_state_r2_state = E->GetFieldID(env, controller_state_class, "r2State", "B");
	session->java_controller_state_left_x = E->GetFieldID(env, controller_state_class, "leftX", "S");
	session->java_controller_state_left_y = E->GetFieldID(env, controller_state_class, "leftY", "S");
	session->java_controller_state_right_x = E->GetFieldID(env, controller_state_class, "rightX", "S");
	session->java_controller_state_right_y = E->GetFieldID(env, controller_state_class, "rightY", "S");

	chiaki_session_set_event_cb(&session->session, android_chiaki_event_cb, session);
	chiaki_session_set_video_sample_cb(&session->session, android_chiaki_video_decoder_video_sample, &session->video_decoder);

	ChiakiAudioSink audio_sink;
	android_chiaki_audio_decoder_get_sink(&session->audio_decoder, &audio_sink);
	chiaki_session_set_audio_sink(&session->session, &audio_sink);

beach:
	free(host_str);
	E->SetIntField(env, result, E->GetFieldID(env, result_class, "errorCode", "I"), (jint)err);
	E->SetLongField(env, result, E->GetFieldID(env, result_class, "ptr", "J"), (jlong)session);
}

JNIEXPORT void JNICALL JNI_FCN(sessionFree)(JNIEnv *env, jobject obj, jlong ptr)
{
	AndroidChiakiSession *session = (AndroidChiakiSession *)ptr;
	CHIAKI_LOGI(&global_log, "Shutting down JNI Session");
	if(!session)
		return;
	chiaki_session_fini(&session->session);
	free(session);
	android_chiaki_video_decoder_fini(&session->video_decoder);
	android_chiaki_audio_decoder_fini(&session->audio_decoder);
	android_chiaki_audio_output_free(session->audio_output);
	E->DeleteGlobalRef(env, session->java_session);
	E->DeleteGlobalRef(env, session->java_session_class);
	CHIAKI_LOGI(&global_log, "JNI Session has quit");
}

JNIEXPORT jint JNICALL JNI_FCN(sessionStart)(JNIEnv *env, jobject obj, jlong ptr)
{
	AndroidChiakiSession *session = (AndroidChiakiSession *)ptr;
	CHIAKI_LOGI(&global_log, "Start JNI Session");
	return chiaki_session_start(&session->session);
}

JNIEXPORT jint JNICALL JNI_FCN(sessionStop)(JNIEnv *env, jobject obj, jlong ptr)
{
	AndroidChiakiSession *session = (AndroidChiakiSession *)ptr;
	CHIAKI_LOGI(&global_log, "Stop JNI Session");
	return chiaki_session_stop(&session->session);
}

JNIEXPORT jint JNICALL JNI_FCN(sessionJoin)(JNIEnv *env, jobject obj, jlong ptr)
{
	AndroidChiakiSession *session = (AndroidChiakiSession *)ptr;
	CHIAKI_LOGI(&global_log, "Join JNI Session");
	return chiaki_session_join(&session->session);
}

JNIEXPORT void JNICALL JNI_FCN(sessionSetSurface)(JNIEnv *env, jobject obj, jlong ptr, jobject surface)
{
	AndroidChiakiSession *session = (AndroidChiakiSession *)ptr;
	android_chiaki_video_decoder_set_surface(&session->video_decoder, env, surface);
}

JNIEXPORT void JNICALL JNI_FCN(sessionSetControllerState)(JNIEnv *env, jobject obj, jlong ptr, jobject controller_state_java)
{
	AndroidChiakiSession *session = (AndroidChiakiSession *)ptr;
	ChiakiControllerState controller_state = { 0 };
	controller_state.buttons = (uint32_t)E->GetIntField(env, controller_state_java, session->java_controller_state_buttons);
	controller_state.l2_state = (uint8_t)E->GetByteField(env, controller_state_java, session->java_controller_state_l2_state);
	controller_state.r2_state = (uint8_t)E->GetByteField(env, controller_state_java, session->java_controller_state_r2_state);
	controller_state.left_x = (int16_t)E->GetShortField(env, controller_state_java, session->java_controller_state_left_x);
	controller_state.left_y = (int16_t)E->GetShortField(env, controller_state_java, session->java_controller_state_left_y);
	controller_state.right_x = (int16_t)E->GetShortField(env, controller_state_java, session->java_controller_state_right_x);
	controller_state.right_y = (int16_t)E->GetShortField(env, controller_state_java, session->java_controller_state_right_y);
	chiaki_session_set_controller_state(&session->session, &controller_state);
}

JNIEXPORT void JNICALL JNI_FCN(sessionSetLoginPin)(JNIEnv *env, jobject obj, jlong ptr, jstring pin_java)
{
	AndroidChiakiSession *session = (AndroidChiakiSession *)ptr;
	const char *pin = E->GetStringUTFChars(env, pin_java, NULL);
	chiaki_session_set_login_pin(&session->session, (const uint8_t *)pin, strlen(pin));
	E->ReleaseStringUTFChars(env, pin_java, pin);
}

typedef struct android_discovery_service_t
{
	ChiakiDiscoveryService service;
	jobject java_service;
	jclass java_service_class;
	jmethodID java_service_hosts_updated_meth;

	jclass host_class;
	jmethodID host_ctor;
	jobject host_state_unknown;
	jobject host_state_ready;
	jobject host_state_standby;
} AndroidDiscoveryService;

static void android_discovery_service_cb(ChiakiDiscoveryHost *hosts, size_t hosts_count, void *user)
{
	AndroidDiscoveryService *service = user;

	CHIAKI_LOGI(&global_log, "JNI Discovery Callback got %llu hosts", (unsigned long long)hosts_count);

	JNIEnv *env = attach_thread_jni();
	if(!env)
		return;

	jobjectArray r = E->NewObjectArray(env, hosts_count, service->host_class, NULL);

	for(size_t i=0; i<hosts_count; i++)
	{
		jobject state;
		ChiakiDiscoveryHost *host = hosts + i;
		switch(host->state)
		{
			case CHIAKI_DISCOVERY_HOST_STATE_STANDBY:
				state = service->host_state_standby;
				break;
			case CHIAKI_DISCOVERY_HOST_STATE_READY:
				state = service->host_state_ready;
				break;
			default:
				state = service->host_state_unknown;
				break;
		}

		jobject o = E->NewObject(env, service->host_class, service->host_ctor,
				state,
				host->host_request_port,
				jnistr_from_ascii(env, host->host_addr),
				jnistr_from_ascii(env, host->system_version),
				jnistr_from_ascii(env, host->device_discovery_protocol_version),
				jnistr_from_ascii(env, host->host_name),
				jnistr_from_ascii(env, host->host_type),
				jnistr_from_ascii(env, host->host_id),
				jnistr_from_ascii(env, host->running_app_titleid),
				jnistr_from_ascii(env, host->running_app_name));

		E->SetObjectArrayElement(env, r, i, o);
	}

	E->CallVoidMethod(env, service->java_service, service->java_service_hosts_updated_meth, r);

	(*global_vm)->DetachCurrentThread(global_vm);
}

static ChiakiErrorCode sockaddr_from_java(JNIEnv *env, jobject /*InetSocketAddress*/ sockaddr_obj, struct sockaddr **addr, size_t *addr_size)
{
	jclass sockaddr_class = E->GetObjectClass(env, sockaddr_obj);
	uint16_t port = (uint16_t)E->CallIntMethod(env, sockaddr_obj, E->GetMethodID(env, sockaddr_class, "getPort", "()I"));
	jobject addr_obj = E->CallObjectMethod(env, sockaddr_obj, E->GetMethodID(env, sockaddr_class, "getAddress", "()Ljava/net/InetAddress;"));
	jclass addr_class = E->GetObjectClass(env, addr_obj);
	jbyteArray addr_byte_array = E->CallObjectMethod(env, addr_obj, E->GetMethodID(env, addr_class, "getAddress", "()[B"));
	jsize addr_byte_array_len = E->GetArrayLength(env, addr_byte_array);

	if(addr_byte_array_len == 4)
	{
		struct sockaddr_in *inaddr = CHIAKI_NEW(struct sockaddr_in);
		if(!inaddr)
			return CHIAKI_ERR_MEMORY;
		memset(inaddr, 0, sizeof(*inaddr));
		inaddr->sin_family = AF_INET;
		jbyte *bytes = E->GetByteArrayElements(env, addr_byte_array, NULL);
		memcpy(&inaddr->sin_addr.s_addr, bytes, sizeof(inaddr->sin_addr.s_addr));
		E->ReleaseByteArrayElements(env, addr_byte_array, bytes, JNI_ABORT);
		inaddr->sin_port = htons(port);

		*addr = (struct sockaddr *)inaddr;
		*addr_size = sizeof(*inaddr);
	}
	else if(addr_byte_array_len == 0x10)
	{
		struct sockaddr_in6 *inaddr6 = CHIAKI_NEW(struct sockaddr_in6);
		if(!inaddr6)
			return CHIAKI_ERR_MEMORY;
		memset(inaddr6, 0, sizeof(*inaddr6));
		inaddr6->sin6_family = AF_INET6;
		jbyte *bytes = E->GetByteArrayElements(env, addr_byte_array, NULL);
		memcpy(&inaddr6->sin6_addr.in6_u, bytes, sizeof(inaddr6->sin6_addr.in6_u));
		E->ReleaseByteArrayElements(env, addr_byte_array, bytes, JNI_ABORT);
		inaddr6->sin6_port = htons(port);

		*addr = (struct sockaddr *)inaddr6;
		*addr_size = sizeof(*inaddr6);
	}
	else
		return CHIAKI_ERR_INVALID_DATA;

	return CHIAKI_ERR_SUCCESS;
}

JNIEXPORT void JNICALL JNI_FCN(discoveryServiceCreate)(JNIEnv *env, jobject obj, jobject result, jobject options_obj, jobject java_service)
{
	jclass result_class = E->GetObjectClass(env, result);
	ChiakiErrorCode err = CHIAKI_ERR_SUCCESS;
	ChiakiDiscoveryServiceOptions options = { 0 };

	AndroidDiscoveryService *service = CHIAKI_NEW(AndroidDiscoveryService);
	if(!service)
	{
		err = CHIAKI_ERR_MEMORY;
		goto beach;
	}

	jclass options_class = E->GetObjectClass(env, options_obj);

	options.hosts_max = (size_t)E->GetLongField(env, options_obj, E->GetFieldID(env, options_class, "hostsMax", "J"));
	options.host_drop_pings = (uint64_t)E->GetLongField(env, options_obj, E->GetFieldID(env, options_class, "hostDropPings", "J"));
	options.ping_ms = (uint64_t)E->GetLongField(env, options_obj, E->GetFieldID(env, options_class, "pingMs", "J"));
	options.cb = android_discovery_service_cb;
	options.cb_user = service;

	err = sockaddr_from_java(env, E->GetObjectField(env, options_obj, E->GetFieldID(env, options_class, "sendAddr", "Ljava/net/InetSocketAddress;")), &options.send_addr, &options.send_addr_size);
	if(err != CHIAKI_ERR_SUCCESS)
	{
		CHIAKI_LOGE(&global_log, "Failed to get sockaddr from InetSocketAddress");
		goto beach;
	}

	service->java_service = E->NewGlobalRef(env, java_service);
	service->java_service_class = E->GetObjectClass(env, service->java_service);
	service->java_service_hosts_updated_meth = E->GetMethodID(env, service->java_service_class, "hostsUpdated", "([L"BASE_PACKAGE"/DiscoveryHost;)V");

	service->host_class = E->NewGlobalRef(env, E->FindClass(env, BASE_PACKAGE"/DiscoveryHost"));
	service->host_ctor = E->GetMethodID(env, service->host_class, "<init>", "("
		"L"BASE_PACKAGE"/DiscoveryHost$State;"
		"S" // hostRequestPort: UShort
		"Ljava/lang/String;" // hostAddr: String?,
		"Ljava/lang/String;" // systemVersion: String?,
		"Ljava/lang/String;" // deviceDiscoveryProtocolVersion: String?,
		"Ljava/lang/String;" // hostName: String?,
		"Ljava/lang/String;" // hostType: String?,
		"Ljava/lang/String;" // hostId: String?,
		"Ljava/lang/String;" // runningAppTitleid: String?,
		"Ljava/lang/String;" // runningAppName: String?
		")V");

	jclass host_state_class = E->FindClass(env, BASE_PACKAGE"/DiscoveryHost$State");
	service->host_state_unknown = E->NewGlobalRef(env, E->GetStaticObjectField(env, host_state_class, E->GetStaticFieldID(env, host_state_class, "UNKNOWN", "L"BASE_PACKAGE"/DiscoveryHost$State;")));
	service->host_state_standby = E->NewGlobalRef(env, E->GetStaticObjectField(env, host_state_class, E->GetStaticFieldID(env, host_state_class, "STANDBY", "L"BASE_PACKAGE"/DiscoveryHost$State;")));
	service->host_state_ready = E->NewGlobalRef(env, E->GetStaticObjectField(env, host_state_class, E->GetStaticFieldID(env, host_state_class, "READY", "L"BASE_PACKAGE"/DiscoveryHost$State;")));


	err = chiaki_discovery_service_init(&service->service, &options, &global_log);
	if(err != CHIAKI_ERR_SUCCESS)
	{
		CHIAKI_LOGE(&global_log, "Failed to create discovery service (JNI)");
		E->DeleteGlobalRef(env, service->java_service);
		E->DeleteGlobalRef(env, service->host_state_unknown);
		E->DeleteGlobalRef(env, service->host_state_standby);
		E->DeleteGlobalRef(env, service->host_state_ready);
		E->DeleteGlobalRef(env, service->host_class);
		free(service);
		goto beach;
	}

beach:
	free(options.send_addr);
	E->SetIntField(env, result, E->GetFieldID(env, result_class, "errorCode", "I"), (jint)err);
	E->SetLongField(env, result, E->GetFieldID(env, result_class, "ptr", "J"), (jlong)service);
}

JNIEXPORT void JNICALL JNI_FCN(discoveryServiceFree)(JNIEnv *env, jobject obj, jlong ptr)
{
	AndroidDiscoveryService *service = (AndroidDiscoveryService *)ptr;
	if(!service)
		return;
	chiaki_discovery_service_fini(&service->service);
	E->DeleteGlobalRef(env, service->java_service);
	E->DeleteGlobalRef(env, service->host_state_unknown);
	E->DeleteGlobalRef(env, service->host_state_standby);
	E->DeleteGlobalRef(env, service->host_state_ready);
	E->DeleteGlobalRef(env, service->host_class);
	free(service);
}

typedef struct android_chiaki_regist_t
{
	AndroidChiakiLog log;
	jobject java_regist;
	ChiakiRegist regist;
} AndroidChiakiRegist;

static void android_chiaki_regist_cb(ChiakiRegistEvent *event, void *user)
{
	// TODO
}

JNIEXPORT void JNICALL JNI_FCN(registStart)(JNIEnv *env, jobject obj, jobject result, jobject regist_info_obj, jobject log_obj, jobject java_regist)
{
	jclass result_class = E->GetObjectClass(env, result);
	ChiakiErrorCode err = CHIAKI_ERR_SUCCESS;
	AndroidChiakiRegist *regist = CHIAKI_NEW(AndroidChiakiRegist);
	if(!regist)
	{
		err = CHIAKI_ERR_MEMORY;
		goto beach;
	}

	android_chiaki_log_init(&regist->log, env, log_obj);

	regist->java_regist = E->NewGlobalRef(env, java_regist);

	jclass regist_info_class = E->GetObjectClass(env, regist_info_obj);
	jstring host_string = E->GetObjectField(env, regist_info_obj, E->GetFieldID(env, regist_info_class, "host", "Ljava/lang/String;"));
	jboolean broadcast = E->GetBooleanField(env, regist_info_obj, E->GetFieldID(env, regist_info_class, "broadcast", "Z"));
	jstring psn_id_string = E->GetObjectField(env, regist_info_obj, E->GetFieldID(env, regist_info_class, "psnId", "Ljava/lang/String;"));
	jint pin = E->GetIntField(env, regist_info_obj, E->GetFieldID(env, regist_info_class, "pin", "I"));

	ChiakiRegistInfo regist_info;
	regist_info.host = E->GetStringUTFChars(env, host_string, NULL);
	regist_info.broadcast = broadcast;
	// TODO regist_info.psn_id = E->GetStringUTFChars(env, psn_id_string, NULL);
	regist_info.pin = (uint32_t)pin;

	err = chiaki_regist_start(&regist->regist, &regist->log.log, &regist_info, android_chiaki_regist_cb, regist);

	E->ReleaseStringUTFChars(env, host_string, regist_info.host);
	// TODO E->ReleaseStringUTFChars(env, psn_id_string, regist_info.psn_id);

	if(err != CHIAKI_ERR_SUCCESS)
	{
		android_chiaki_log_fini(&regist->log, env);
		E->DeleteGlobalRef(env, regist->java_regist);
		free(regist);
		regist = NULL;
	}

beach:
	E->SetIntField(env, result, E->GetFieldID(env, result_class, "errorCode", "I"), (jint)err);
	E->SetLongField(env, result, E->GetFieldID(env, result_class, "ptr", "J"), (jlong)regist);
}

JNIEXPORT void JNICALL JNI_FCN(registStop)(JNIEnv *env, jobject obj, jlong ptr)
{
	AndroidChiakiRegist *regist = (AndroidChiakiRegist *)ptr;
	chiaki_regist_stop(&regist->regist);
}

JNIEXPORT void JNICALL JNI_FCN(registFree)(JNIEnv *env, jobject obj, jlong ptr)
{
	AndroidChiakiRegist *regist = (AndroidChiakiRegist *)ptr;
	chiaki_regist_fini(&regist->regist);
	android_chiaki_log_fini(&regist->log, env);
	E->DeleteGlobalRef(env, regist->java_regist);
	free(regist);
}
