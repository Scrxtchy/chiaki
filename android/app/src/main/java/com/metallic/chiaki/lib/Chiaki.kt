package com.metallic.chiaki.lib

import android.os.Parcelable
import android.util.Log
import android.view.Surface
import kotlinx.android.parcel.Parcelize
import java.lang.Exception
import java.net.InetSocketAddress
import kotlin.math.abs

@Parcelize
data class ConnectVideoProfile(
	val width: Int,
	val height: Int,
	val maxFPS: Int,
	val bitrate: Int
): Parcelable

@Parcelize
data class ConnectInfo(
	val host: String,
	val registKey: ByteArray,
	val morning: ByteArray,
	val videoProfile: ConnectVideoProfile
): Parcelable

private class ChiakiNative
{
	data class CreateResult(var errorCode: Int, var ptr: Long)
	companion object
	{
		init
		{
			System.loadLibrary("chiaki-jni")
		}
		@JvmStatic external fun errorCodeToString(value: Int): String
		@JvmStatic external fun quitReasonToString(value: Int): String
		@JvmStatic external fun quitReasonIsStopped(value: Int): Boolean
		@JvmStatic external fun sessionCreate(result: CreateResult, connectInfo: ConnectInfo, javaSession: Session)
		@JvmStatic external fun sessionFree(ptr: Long)
		@JvmStatic external fun sessionStart(ptr: Long): Int
		@JvmStatic external fun sessionStop(ptr: Long): Int
		@JvmStatic external fun sessionJoin(ptr: Long): Int
		@JvmStatic external fun sessionSetSurface(ptr: Long, surface: Surface)
		@JvmStatic external fun sessionSetControllerState(ptr: Long, controllerState: ControllerState)
		@JvmStatic external fun sessionSetLoginPin(ptr: Long, pin: String)
		@JvmStatic external fun discoveryServiceCreate(result: CreateResult, options: DiscoveryServiceOptions, javaService: DiscoveryService)
		@JvmStatic external fun discoveryServiceFree(ptr: Long)
		@JvmStatic external fun registStart(result: CreateResult, registInfo: RegistInfo, javaLog: ChiakiLog, javaRegist: Regist)
		@JvmStatic external fun registStop(ptr: Long)
		@JvmStatic external fun registFree(ptr: Long)
	}
}

class ErrorCode(val value: Int)
{
	override fun toString() = ChiakiNative.errorCodeToString(value)
	var isSuccess = value == 0
}

class ChiakiLog(val levelMask: Int, val callback: (level: Int, text: String) -> Unit)
{
	companion object
	{
		fun formatLog(level: Int, text: String) =
			"[${when(level)
				{
					Level.DEBUG.value -> "D"
					Level.VERBOSE.value -> "V"
					Level.INFO.value -> "I"
					Level.WARNING.value -> "W"
					Level.ERROR.value -> "E"
					else -> "?"
				}
			}] $text"
	}

	enum class Level(val value: Int)
	{
		DEBUG(1 shl 4),
		VERBOSE(1 shl 3),
		INFO(1 shl 2),
		WARNING(1 shl 1),
		ERROR(1 shl 0),
		ALL(0.inv())
	}

	private fun log(level: Int, text: String)
	{
		callback(level, text)
	}
}

private fun maxAbs(a: Short, b: Short) = if(abs(a.toInt()) > abs(b.toInt())) a else b

data class ControllerState constructor(
	var buttons: UInt = 0U,
	var l2State: UByte = 0U,
	var r2State: UByte = 0U,
	var leftX: Short = 0,
	var leftY: Short = 0,
	var rightX: Short = 0,
	var rightY: Short = 0
){
	companion object
	{
		val BUTTON_CROSS 		= (1 shl 0).toUInt()
		val BUTTON_MOON 		= (1 shl 1).toUInt()
		val BUTTON_BOX 		= (1 shl 2).toUInt()
		val BUTTON_PYRAMID 	= (1 shl 3).toUInt()
		val BUTTON_DPAD_LEFT 	= (1 shl 4).toUInt()
		val BUTTON_DPAD_RIGHT	= (1 shl 5).toUInt()
		val BUTTON_DPAD_UP 	= (1 shl 6).toUInt()
		val BUTTON_DPAD_DOWN 	= (1 shl 7).toUInt()
		val BUTTON_L1 		= (1 shl 8).toUInt()
		val BUTTON_R1 		= (1 shl 9).toUInt()
		val BUTTON_L3			= (1 shl 10).toUInt()
		val BUTTON_R3			= (1 shl 11).toUInt()
		val BUTTON_OPTIONS 	= (1 shl 12).toUInt()
		val BUTTON_SHARE 		= (1 shl 13).toUInt()
		val BUTTON_TOUCHPAD	= (1 shl 14).toUInt()
		val BUTTON_PS			= (1 shl 15).toUInt()
	}

	infix fun or(o: ControllerState) = ControllerState(
		buttons = buttons or o.buttons,
		l2State = maxOf(l2State, o.l2State),
		r2State = maxOf(r2State, o.r2State),
		leftX = maxAbs(leftX, o.leftX),
		leftY = maxAbs(leftY, o.leftY),
		rightX = maxAbs(rightX, o.rightX),
		rightY = maxAbs(rightY, o.rightY)
	)
}

class QuitReason(val value: Int)
{
	override fun toString() = ChiakiNative.quitReasonToString(value)

	/**
	 * whether the reason is CHIAKI_QUIT_REASON_STOPPED
	 */
	val isStopped = ChiakiNative.quitReasonIsStopped(value)
}

sealed class Event
object ConnectedEvent: Event()
data class LoginPinRequestEvent(val pinIncorrect: Boolean): Event()
data class QuitEvent(val reason: QuitReason, val reasonString: String?): Event()

class CreateError(val errorCode: ErrorCode): Exception("Failed to create a native object: $errorCode")

class Session(connectInfo: ConnectInfo)
{
	interface EventCallback
	{
		fun sessionEvent(event: Event)
	}

	private var nativePtr: Long
	var eventCallback: ((event: Event) -> Unit)? = null

	init
	{
		val result = ChiakiNative.CreateResult(0, 0)
		ChiakiNative.sessionCreate(result, connectInfo, this)
		val errorCode = ErrorCode(result.errorCode)
		if(!errorCode.isSuccess)
			throw CreateError(errorCode)
		nativePtr = result.ptr
	}

	fun start() = ErrorCode(ChiakiNative.sessionStart(nativePtr))
	fun stop() = ErrorCode(ChiakiNative.sessionStop(nativePtr))

	fun dispose()
	{
		if(nativePtr == 0L)
			return
		ChiakiNative.sessionJoin(nativePtr)
		ChiakiNative.sessionFree(nativePtr)
		nativePtr = 0L
	}

	private fun event(event: Event)
	{
		eventCallback?.let { it(event) }
	}

	private fun eventConnected()
	{
		event(ConnectedEvent)
	}

	private fun eventLoginPinRequest(pinIncorrect: Boolean)
	{
		event(LoginPinRequestEvent(pinIncorrect))
	}

	private fun eventQuit(reasonValue: Int, reasonString: String?)
	{
		event(QuitEvent(QuitReason(reasonValue), reasonString))
	}

	fun setSurface(surface: Surface)
	{
		ChiakiNative.sessionSetSurface(nativePtr, surface)
	}

	fun setControllerState(controllerState: ControllerState)
	{
		ChiakiNative.sessionSetControllerState(nativePtr, controllerState)
	}

	fun setLoginPin(pin: String)
	{
		ChiakiNative.sessionSetLoginPin(nativePtr, pin)
	}
}

data class DiscoveryHost(
	val state: State,
	val hostRequestPort: UShort,
	val hostAddr: String?,
	val systemVersion: String?,
	val deviceDiscoveryProtocolVersion: String?,
	val hostName: String?,
	val hostType: String?,
	val hostId: String?,
	val runningAppTitleid: String?,
	val runningAppName: String?)
{
	enum class State
	{
		UNKNOWN,
		READY,
		STANDBY
	}
}


data class DiscoveryServiceOptions(
	val hostsMax: ULong,
	val hostDropPings: ULong,
	val pingMs: ULong,
	val sendAddr: InetSocketAddress
)

class DiscoveryService(
	options: DiscoveryServiceOptions,
	val callback: ((hosts: List<DiscoveryHost>) -> Unit)?)
{
	private var nativePtr: Long

	init
	{
		val result = ChiakiNative.CreateResult(0, 0)
		ChiakiNative.discoveryServiceCreate(result, options, this)
		val errorCode = ErrorCode(result.errorCode)
		if(!errorCode.isSuccess)
			throw CreateError(errorCode)
		nativePtr = result.ptr
	}

	fun dispose()
	{
		if(nativePtr == 0L)
			return
		ChiakiNative.discoveryServiceFree(nativePtr)
		nativePtr = 0L
	}

	private fun hostsUpdated(hosts: Array<DiscoveryHost>)
	{
		val hostsList = hosts.toList()
		Log.i("Chiaki", "got hosts from native: $hostsList")
		callback?.let { it(hostsList) }
	}

}


data class RegistInfo(
	val host: String,
	val broadcast: Boolean,
	val psnId: String, // TODO: this is outdated now
	val pin: UInt
)

data class RegistHost(
	val apSsid: String,
	val apBssid: String,
	val apKey: String,
	val apName: String,
	val ps4Mac: ByteArray,
	val ps4Nickname: String,
	val rpRegistKey: ByteArray,
	val rpKeyType: UInt,
	val rpKey: ByteArray
)

sealed class RegistEvent
object RegistEventCanceled: RegistEvent()
object RegistEventFailed: RegistEvent()
class RegistEventSuccess(val host: RegistHost): RegistEvent()

class Regist(
	info: RegistInfo,
	log: ChiakiLog,
	val callback: (RegistEvent) -> Unit
)
{
	private var nativePtr: Long

	init
	{
		val result = ChiakiNative.CreateResult(0, 0)
		ChiakiNative.registStart(result, info, log, this)
		val errorCode = ErrorCode(result.errorCode)
		if(!errorCode.isSuccess)
			throw CreateError(errorCode)
		nativePtr = result.ptr
	}

	fun stop()
	{
		ChiakiNative.registStop(nativePtr)
	}

	fun dispose()
	{
		if(nativePtr == 0L)
			return
		ChiakiNative.registFree(nativePtr)
		nativePtr = 0L
	}

	private fun event(event: RegistEvent)
	{
		callback(event)
	}
}
