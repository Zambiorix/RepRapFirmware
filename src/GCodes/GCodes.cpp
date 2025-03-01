/****************************************************************************************************

 RepRapFirmware - G Codes

 This class interprets G Codes from one or more sources, and calls the functions in Move, Heat etc
 that drive the machine to do what the G Codes command.

 Most of the functions in here are designed not to wait, and they return a boolean.  When you want them to do
 something, you call them.  If they return false, the machine can't do what you want yet.  So you go away
 and do something else.  Then you try again.  If they return true, the thing you wanted done has been done.

 -----------------------------------------------------------------------------------------------------

 Version 0.1

 13 February 2013

 Adrian Bowyer
 RepRap Professional Ltd
 http://reprappro.com

 Licence: GPL

 ****************************************************************************************************/

#include "GCodes.h"

#include "GCodeBuffer/GCodeBuffer.h"
#include "GCodeQueue.h"
#include <Heating/Heat.h>
#include <Platform/Platform.h>
#include <Movement/Move.h>
#include <Platform/Scanner.h>
#include <PrintMonitor/PrintMonitor.h>
#include <Platform/RepRap.h>
#include <Platform/Tasks.h>
#include <Platform/Event.h>
#include <Tools/Tool.h>
#include <Endstops/ZProbe.h>
#include <ObjectModel/Variable.h>

#if SUPPORT_LED_STRIPS
# include <Fans/LedStripDriver.h>
#endif

#if HAS_SBC_INTERFACE
# include <SBC/SbcInterface.h>
#endif

#if SUPPORT_REMOTE_COMMANDS
# include <CAN/CanInterface.h>
#endif

#if HAS_AUX_DEVICES
// Support for emergency stop from PanelDue
bool GCodes::emergencyStopCommanded = false;

void GCodes::CommandEmergencyStop(AsyncSerial *p) noexcept
{
	emergencyStopCommanded = true;
}
#endif

GCodes::GCodes(Platform& p) noexcept :
#if HAS_AUX_DEVICES && ALLOW_ARBITRARY_PANELDUE_PORT
	serialChannelForPanelDueFlashing(1),
#endif
	platform(p), machineType(MachineType::fff), active(false)
#if HAS_VOLTAGE_MONITOR
	, powerFailScript(nullptr)
#endif
	, isFlashing(false),
#if SUPPORT_PANELDUE_FLASH
	isFlashingPanelDue(false),
#endif
	lastWarningMillis(0)
#if HAS_MASS_STORAGE
	, sdTimingFile(nullptr)
#endif
{
#if HAS_MASS_STORAGE || HAS_EMBEDDED_FILES
	fileBeingHashed = nullptr;
	FileGCodeInput * const fileInput = new FileGCodeInput();
#else
	FileGCodeInput * const fileInput = nullptr;
#endif
	fileGCode = new GCodeBuffer(GCodeChannel::File, nullptr, fileInput, GenericMessage);

# if SUPPORT_HTTP || HAS_SBC_INTERFACE
	httpInput = new NetworkGCodeInput();
	httpGCode = new GCodeBuffer(GCodeChannel::HTTP, httpInput, fileInput, HttpMessage);
# else
	httpGCode = nullptr;
# endif // SUPPORT_HTTP || HAS_SBC_INTERFACE
# if SUPPORT_TELNET || HAS_SBC_INTERFACE
	telnetInput = new NetworkGCodeInput();
	telnetGCode = new GCodeBuffer(GCodeChannel::Telnet, telnetInput, fileInput, TelnetMessage, Compatibility::Marlin);
# else
	telnetGCode = nullptr;
# endif // SUPPORT_TELNET || HAS_SBC_INTERFACE
#if defined(SERIAL_MAIN_DEVICE)
# if SAME5x
	// SAME5x USB driver already uses an efficient buffer for receiving data from USB
	StreamGCodeInput * const usbInput = new StreamGCodeInput(SERIAL_MAIN_DEVICE);
# else
	// Old USB driver is inefficient when read in single-character mode
	BufferedStreamGCodeInput * const usbInput = new BufferedStreamGCodeInput(SERIAL_MAIN_DEVICE);
# endif
	usbGCode = new GCodeBuffer(GCodeChannel::USB, usbInput, fileInput, UsbMessage, Compatibility::Marlin);
#elif HAS_SBC_INTERFACE
	usbGCode = new GCodeBuffer(GCodeChannel::USB, nullptr, fileInput, UsbMessage, Compatbility::marlin);
#else
	usbGCode = nullptr;
#endif

#if HAS_AUX_DEVICES
	StreamGCodeInput * const auxInput = new StreamGCodeInput(SERIAL_AUX_DEVICE);
	auxGCode = new GCodeBuffer(GCodeChannel::Aux, auxInput, fileInput, AuxMessage);
#elif HAS_SBC_INTERFACE
	auxGCode = new GCodeBuffer(GCodeChannel::Aux, nullptr, fileInput, AuxMessage);
#else
	auxGCode = nullptr;
#endif

	triggerGCode = new GCodeBuffer(GCodeChannel::Trigger, nullptr, fileInput, GenericMessage);

	codeQueue = new GCodeQueue();
	queuedGCode = new GCodeBuffer(GCodeChannel::Queue, codeQueue, fileInput, GenericMessage);

#if SUPPORT_12864_LCD || HAS_SBC_INTERFACE
	lcdGCode = new GCodeBuffer(GCodeChannel::LCD, nullptr, fileInput, LcdMessage);
#else
	lcdGCode = nullptr;
#endif

#if HAS_SBC_INTERFACE
	spiGCode = new GCodeBuffer(GCodeChannel::SBC, nullptr, fileInput, GenericMessage);
#else
	spiGCode = nullptr;
#endif
	daemonGCode = new GCodeBuffer(GCodeChannel::Daemon, nullptr, fileInput, GenericMessage);
#if defined(SERIAL_AUX2_DEVICE)
	StreamGCodeInput * const aux2Input = new StreamGCodeInput(SERIAL_AUX2_DEVICE);
	aux2GCode = new GCodeBuffer(GCodeChannel::Aux2, aux2Input, fileInput, Aux2Message);
#elif HAS_SBC_INTERFACE
	aux2GCode = new GCodeBuffer(GCodeChannel::Aux2, nullptr, fileInput, Aux2Message);
#else
	aux2GCode = nullptr;
#endif
	autoPauseGCode = new GCodeBuffer(GCodeChannel::Autopause, nullptr, fileInput, GenericMessage);
}

void GCodes::Exit() noexcept
{
	active = false;
}

void GCodes::Init() noexcept
{
	numVisibleAxes = numTotalAxes = XYZ_AXES;			// must set this up before calling Reset()
	memset(axisLetters, 0, sizeof(axisLetters));
	axisLetters[0] = 'X';
	axisLetters[1] = 'Y';
	axisLetters[2] = 'Z';

	numExtruders = 0;

	Reset();

	virtualExtruderPosition = rawExtruderTotal = 0.0;
	for (float& f : rawExtruderTotalByDrive)
	{
		f = 0.0;
	}

	runningConfigFile = daemonRunning = false;
	m501SeenInConfigFile = false;
	doingToolChange = false;
	active = true;
	limitAxes = noMovesBeforeHoming = true;
	SetAllAxesNotHomed();

	lastDefaultFanSpeed = 0.0;

	lastAuxStatusReportType = -1;						// no status reports requested yet

	laserMaxPower = DefaultMaxLaserPower;
	laserPowerSticky = false;

#if SUPPORT_SCANNER
	reprap.GetScanner().SetGCodeBuffer(usbGCode);
#endif

#if SUPPORT_LED_STRIPS
	LedStripDriver::Init();
#endif

#if HAS_AUX_DEVICES && !defined(__LPC17xx__)
	SERIAL_AUX_DEVICE.SetInterruptCallback(GCodes::CommandEmergencyStop);
#endif
}

// This is called from Init and when doing an emergency stop
void GCodes::Reset() noexcept
{
	// Here we could reset the input sources as well, but this would mess up M122\nM999
	// because both codes are sent at once from the web interface. Hence we don't do this here.
	for (GCodeBuffer *gb : gcodeSources)
	{
		if (gb != nullptr)
		{
			gb->Reset();
		}
	}

	if (auxGCode != nullptr)
	{
		auxGCode->SetCommsProperties(1);				// by default, we require a checksum on the aux port
	}

	nextGcodeSource = 0;

#if HAS_MASS_STORAGE || HAS_EMBEDDED_FILES
	fileToPrint.Close();
#endif
	speedFactor = 1.0;

	for (size_t i = 0; i < MaxExtruders; ++i)
	{
		extrusionFactors[i] = volumetricExtrusionFactors[i] = 1.0;
	}

	for (size_t i = 0; i < MaxAxes; ++i)
	{
		axisScaleFactors[i] = 1.0;
		for (size_t j = 0; j < NumCoordinateSystems; ++j)
		{
			workplaceCoordinates[j][i] = 0.0;
		}
	}

#if SUPPORT_COORDINATE_ROTATION
	g68Angle = g68Centre[0] = g68Centre[1] = 0.0;		// no coordinate rotation
#endif

	moveState.currentCoordinateSystem = 0;

	for (float& f : moveState.coords)
	{
		f = 0.0;										// clear out all axis and extruder coordinates
	}

	ClearMove();

	for (float& f : currentBabyStepOffsets)
	{
		f = 0.0;										// clear babystepping before calling ToolOffsetInverseTransform
	}

	moveState.currentZHop = 0.0;						// clear this before calling ToolOffsetInverseTransform
	newToolNumber = -1;

	moveState.tool = nullptr;
	moveState.virtualExtruderPosition = 0.0;
#if SUPPORT_LASER || SUPPORT_IOBITS
	moveState.laserPwmOrIoBits.Clear();
#endif
	reprap.GetMove().GetKinematics().GetAssumedInitialPosition(numVisibleAxes, moveState.coords);
	ToolOffsetInverseTransform(moveState.coords, moveState.currentUserPosition);
	updateUserPositionGb = nullptr;

	for (RestorePoint& rp : numberedRestorePoints)
	{
		rp.Init();
	}

	for (TriggerItem& tr : triggers)
	{
		tr.Init();
	}
	triggersPending.Clear();

	simulationMode = SimulationMode::off;
	exitSimulationWhenFileComplete = updateFileWhenSimulationComplete = false;
	simulationTime = 0.0;
	lastDuration = 0;

	pauseState = PauseState::notPaused;
	pausedInMacro = false;
#if HAS_VOLTAGE_MONITOR
	isPowerFailPaused = false;
#endif
	doingToolChange = false;
	doingManualBedProbe = false;
#if HAS_MASS_STORAGE || HAS_SBC_INTERFACE || HAS_EMBEDDED_FILES
	fileOffsetToPrint = 0;
	restartMoveFractionDone = 0.0;
#endif
	printFilePositionAtMacroStart = 0;
	deferredPauseCommandPending = nullptr;
	moveState.filePos = noFilePosition;
	firmwareUpdateModuleMap.Clear();
	isFlashing = false;
#if SUPPORT_PANELDUE_FLASH
	isFlashingPanelDue = false;
#endif
	currentZProbeNumber = 0;

	buildObjects.Init();

	codeQueue->Clear();
	cancelWait = isWaiting = displayNoToolWarning = false;

	for (const GCodeBuffer*& gbp : resourceOwners)
	{
		gbp = nullptr;
	}
}

// Return true if any channel other than the daemon is executing a file macro
bool GCodes::DoingFileMacro() const noexcept
{
	for (const GCodeBuffer *gbp : gcodeSources)
	{
		if (gbp != nullptr && gbp != daemonGCode && gbp->IsDoingFileMacro())
		{
			return true;
		}
	}
	return false;
}

// Return true if any channel is waiting for a message acknowledgement
bool GCodes::WaitingForAcknowledgement() const noexcept
{
	for (const GCodeBuffer *gbp : gcodeSources)
	{
		if (gbp != nullptr && gbp->LatestMachineState().waitingForAcknowledgement)
		{
			return true;
		}
	}
	return false;
}

// Return the current position of the file being printed in bytes.
// May return noFilePosition if allowNoFilePos is true
FilePosition GCodes::GetFilePosition(bool allowNoFilePos) const noexcept
{
#if HAS_SBC_INTERFACE
	if (!reprap.UsingSbcInterface())
#endif
	{
#if HAS_MASS_STORAGE || HAS_EMBEDDED_FILES
		const FileData& fileBeingPrinted = fileGCode->OriginalMachineState().fileState;
		if (!fileBeingPrinted.IsLive())
		{
			return allowNoFilePos ? noFilePosition : 0;
		}
#endif
	}

#if HAS_MASS_STORAGE || HAS_EMBEDDED_FILES || HAS_SBC_INTERFACE
	const FilePosition pos = (fileGCode->IsDoingFileMacro())
			? printFilePositionAtMacroStart						// the position before we started executing the macro
				: fileGCode->GetFilePosition();					// the actual position, allowing for bytes cached but not yet processed
	return (pos != noFilePosition || allowNoFilePos) ? pos : 0;
#else
	return allowNoFilePos ? noFilePosition : 0;
#endif
}

// Start running the config file
bool GCodes::RunConfigFile(const char* fileName) noexcept
{
	runningConfigFile = DoFileMacro(*triggerGCode, fileName, false, AsyncSystemMacroCode);
	return runningConfigFile;
}

// Return true if the trigger G-code buffer is busy running config.g or a trigger file
bool GCodes::IsTriggerBusy() const noexcept
{
	return triggerGCode->IsDoingFile();
}

// Copy the feed rate etc. from the channel that was running config.g to the input channels
void GCodes::CheckFinishedRunningConfigFile(GCodeBuffer& gb) noexcept
{
	if (runningConfigFile && gb.GetChannel() == GCodeChannel::Trigger)
	{
		gb.LatestMachineState().GetPrevious()->CopyStateFrom(gb.LatestMachineState());	// so that M83 etc. in  nested file don't get forgotten
		if (gb.LatestMachineState().GetPrevious()->GetPrevious() == nullptr)
		{
			for (GCodeBuffer *gb2 : gcodeSources)
			{
				if (gb2 != nullptr && gb2 != &gb)
				{
					gb2->LatestMachineState().CopyStateFrom(gb.LatestMachineState());
				}
			}
			runningConfigFile = false;
		}
		reprap.InputsUpdated();
	}
}

// Set up to do the first of a possibly multi-tap probe
void GCodes::InitialiseTaps(bool fastThenSlow) noexcept
{
	tapsDone = (fastThenSlow) ? -1 : 0;
	g30zHeightErrorSum = 0.0;
	g30zHeightErrorLowestDiff = 1000.0;
}

void GCodes::Spin() noexcept
{
	if (!active)
	{
		return;
	}

#if HAS_AUX_DEVICES
	if (emergencyStopCommanded)
	{
		DoEmergencyStop();
		while (SERIAL_AUX_DEVICE.read() >= 0) { }
		emergencyStopCommanded = false;
		return;
	}
#endif

	if (updateUserPositionGb != nullptr)
	{
		UpdateCurrentUserPosition(*updateUserPositionGb);
		updateUserPositionGb = nullptr;
	}

	CheckTriggers();

	// The autoPause buffer has priority, so spin that one first. It may have to wait for other buffers to release locks etc.
	(void)SpinGCodeBuffer(*autoPauseGCode);

	// Use round-robin scheduling for the other input sources
	// Scan the GCode input channels until we find one that we can do some useful work with, or we have scanned them all.
	// The idea is that when a single GCode input channel is active, we do some useful work every time we come through this polling loop, not once every N times (N = number of input channels)
	const size_t originalNextGCodeSource = nextGcodeSource;
	do
	{
		GCodeBuffer * const gbp = gcodeSources[nextGcodeSource];
		++nextGcodeSource;													// move on to the next gcode source ready for next time
		if (nextGcodeSource == ARRAY_SIZE(gcodeSources) - 1)				// the last one is autoPauseGCode, so don't do it again
		{
			nextGcodeSource = 0;
		}
		if (gbp != nullptr && (gbp != auxGCode || !IsFlashingPanelDue()))	// skip auxGCode while flashing PanelDue is in progress
		{
			if (SpinGCodeBuffer(*gbp))										// if we did something useful
			{
				break;
			}
		}
	} while (nextGcodeSource != originalNextGCodeSource);


#if HAS_SBC_INTERFACE
	// Need to check if the print has been stopped by the SBC
	if (reprap.UsingSbcInterface() && reprap.GetSbcInterface().IsPrintAborted())
	{
		StopPrint(StopPrintReason::abort);
	}
#endif

	// Check if we need to display a warning
	const uint32_t now = millis();
	if (now - lastWarningMillis >= MinimumWarningInterval)
	{
		if (displayNoToolWarning)
		{
			platform.Message(ErrorMessage, "Attempting to extrude with no tool selected.\n");
			displayNoToolWarning = false;
			lastWarningMillis = now;
		}
	}
}


// Do some work on an input channel, returning true if we did something significant
bool GCodes::SpinGCodeBuffer(GCodeBuffer& gb) noexcept
{
	// Set up a buffer for the reply
	String<GCodeReplyLength> reply;
	bool result;

	MutexLocker gbLock(gb.mutex);
	if (gb.GetState() == GCodeState::normal)
	{
		if (gb.LatestMachineState().messageAcknowledged)
		{
			const bool wasCancelled = gb.LatestMachineState().messageCancelled;
			gb.PopState(true);											// this could fail if the current macro has already been aborted

			if (wasCancelled)
			{
				if (gb.LatestMachineState().GetPrevious() == nullptr)
				{
					StopPrint(StopPrintReason::userCancelled);
				}
				else
				{
					FileMacroCyclesReturn(gb);
				}
			}
			result = wasCancelled;
		}
		else
		{
			result = StartNextGCode(gb, reply.GetRef());
		}
	}
	else
	{
		RunStateMachine(gb, reply.GetRef());                            // execute the state machine
		result = true;													// assume we did something useful (not necessarily true, e.g. could be waiting for movement to stop)
	}

	if ((gb.IsExecuting()
#if HAS_SBC_INTERFACE
		 && !gb.IsSendRequested()
#endif
		) || (isWaiting && !cancelWait)									// this is needed to get reports sent during M109 commands
	   )
	{
		CheckReportDue(gb, reply.GetRef());
	}

	return result;
}

// Start a new gcode, or continue to execute one that has already been started. Return true if we found something significant to do.
bool GCodes::StartNextGCode(GCodeBuffer& gb, const StringRef& reply) noexcept
{
	// There are special rules for fileGCode because it needs to suspend when paused:
	// - if the pause state is paused or resuming, don't execute
	// - if the state is pausing then don't execute, unless we are executing a macro (because it could be the pause macro or filament change macro)
	// - if there is a deferred pause pending, don't execute once we have finished the current macro
	if (&gb == fileGCode
		&& (   pauseState > PauseState::pausing				// paused or resuming
			|| (!gb.IsDoingFileMacro() && (deferredPauseCommandPending != nullptr || pauseState == PauseState::pausing))
		   )
	   )
	{
		// We are paused or pausing, so don't process any more gcodes from the file being printed.
		// There is a potential issue here if fileGCode holds any locks, so unlock everything.
		UnlockAll(gb);
	}
	else if (gb.IsReady() || gb.IsExecuting())
	{
		gb.SetFinished(ActOnCode(gb, reply));
		return true;
	}
	else if (gb.IsDoingFile())
	{
		return DoFilePrint(gb, reply);
	}
	else if (&gb == autoPauseGCode && !gb.LatestMachineState().waitingForAcknowledgement)
	{
		if (Event::StartProcessing())
		{
			ProcessEvent(gb);								// call out to separate function to avoid increasing stack usage of this function
		}
	}
	else if (&gb == daemonGCode
#if SUPPORT_REMOTE_COMMANDS
			 && !CanInterface::InExpansionMode()			// looking for the daemon.g file increases the loop time too much
#endif
			)
	{
		// Delay 1 or 10 seconds, then try to open and run daemon.g. No error if it is not found.
		if (   !reprap.IsProcessingConfig()
			&& gb.DoDwellTime((daemonRunning) ? 10000 : 1000)
		   )
		{
			daemonRunning = true;
			return DoFileMacro(gb, DAEMON_G, false, AsyncSystemMacroCode);
		}
	}
	else
#if SUPPORT_SCANNER
		 if (!(&gb == usbGCode && reprap.GetScanner().IsRegistered()))
#endif
	{
		const bool gotCommand = (gb.GetNormalInput() != nullptr) && gb.GetNormalInput()->FillBuffer(&gb);
		if (gotCommand)
		{
			gb.DecodeCommand();
			bool done;
			try
			{
				done = gb.CheckMetaCommand(reply);
			}
			catch (const GCodeException& e)
			{
				e.GetMessage(reply, &gb);
				HandleReplyPreserveResult(gb, GCodeResult::error, reply.c_str());
				gb.Init();
				return true;
			}

			if (done)
			{
				HandleReplyPreserveResult(gb, GCodeResult::ok, reply.c_str());
				return true;
			}
		}
#if HAS_SBC_INTERFACE
		else if (reprap.UsingSbcInterface())
		{
			return reprap.GetSbcInterface().FillBuffer(gb);
		}
#endif
	}
	return false;
}

// Try to continue with a print from file, returning true if we did anything significant
bool GCodes::DoFilePrint(GCodeBuffer& gb, const StringRef& reply) noexcept
{
#if HAS_SBC_INTERFACE
	if (reprap.UsingSbcInterface())
	{
		if (gb.IsFileFinished())
		{
			if (gb.LatestMachineState().GetPrevious() == nullptr)
			{
				// Finished printing SD card file.
				// We never get here if the file ends in M0 because CancelPrint gets called directly in that case.
				// Don't close the file until all moves have been completed, in case the print gets paused.
				// Also, this keeps the state as 'Printing' until the print really has finished.
				if (LockMovementAndWaitForStandstill(gb))				// wait until movement has finished and deferred command queue has caught up
				{
					StopPrint(StopPrintReason::normalCompletion);
				}
				return true;
			}

			if (!gb.IsMacroFileClosed())
			{
				// Finished a macro or finished processing config.g
				gb.MacroFileClosed();
				CheckFinishedRunningConfigFile(gb);

				// Pop the stack and notify the SBC that we have closed the file
				Pop(gb, false);
				gb.Init();
				gb.LatestMachineState().firstCommandAfterRestart = false;

				// Send a final code response
				if (gb.GetState() == GCodeState::normal)
				{
					UnlockAll(gb);
					if (!gb.LatestMachineState().lastCodeFromSbc || gb.LatestMachineState().macroStartedByCode)
					{
						HandleReply(gb, GCodeResult::ok, "");
					}
					CheckForDeferredPause(gb);
				}
				return true;
			}
			return false;
		}
		else
		{
			if (gb.LatestMachineState().waitingForAcknowledgement && gb.GetNormalInput() != nullptr)
			{
				if (gb.GetNormalInput()->FillBuffer(&gb))
				{
					gb.DecodeCommand();
					return true;
				}
			}
			return reprap.GetSbcInterface().FillBuffer(gb);
		}
	}
	else
#endif
	{
#if HAS_MASS_STORAGE || HAS_EMBEDDED_FILES
		FileData& fd = gb.LatestMachineState().fileState;

		// Do we have more data to process?
		switch (gb.GetFileInput()->ReadFromFile(fd))
		{
		case GCodeInputReadResult::haveData:
			if (gb.GetFileInput()->FillBuffer(&gb))
			{
				bool done;
				try
				{
					done = gb.CheckMetaCommand(reply);
				}
				catch (const GCodeException& e)
				{
					e.GetMessage(reply, &gb);
					HandleReplyPreserveResult(gb, GCodeResult::error, reply.c_str());
					gb.Init();
					AbortPrint(gb);
					return true;
				}

				if (done)
				{
					HandleReplyPreserveResult(gb, GCodeResult::ok, reply.c_str());
				}
				else
				{
					gb.DecodeCommand();
					if (gb.IsReady())
					{
						gb.SetFinished(ActOnCode(gb, reply));
					}
				}
			}
			return true;

		case GCodeInputReadResult::error:
		default:
			AbortPrint(gb);
			return true;

		case GCodeInputReadResult::noData:
			// We have reached the end of the file. Check for the last line of gcode not ending in newline.
			if (gb.FileEnded())							// append a newline if necessary and deal with any pending file write
			{
				bool done;
				try
				{
					done = gb.CheckMetaCommand(reply);
				}
				catch (const GCodeException& e)
				{
					e.GetMessage(reply, &gb);
					HandleReply(gb, GCodeResult::error, reply.c_str());
					gb.Init();
					AbortPrint(gb);
					return true;
				}

				if (done)
				{
					HandleReply(gb, GCodeResult::ok, reply.c_str());
				}
				else
				{
					gb.DecodeCommand();
					if (gb.IsReady())
					{
						gb.SetFinished(ActOnCode(gb, reply));
					}
				}
				return true;
			}

			gb.Init();								// mark buffer as empty

			if (gb.LatestMachineState().GetPrevious() == nullptr)
			{
				// Finished printing SD card file.
				// We never get here if the file ends in M0 because CancelPrint gets called directly in that case.
				// Don't close the file until all moves have been completed, in case the print gets paused.
				// Also, this keeps the state as 'Printing' until the print really has finished.
				if (LockMovementAndWaitForStandstill(gb))					// wait until movement has finished and deferred command queue has caught up
				{
					StopPrint(StopPrintReason::normalCompletion);
				}
			}
			else
			{
				// Finished a macro or finished processing config.g
				gb.GetFileInput()->Reset(fd);
				fd.Close();
				CheckFinishedRunningConfigFile(gb);
				Pop(gb, false);
				gb.Init();
				if (gb.GetState() == GCodeState::normal)
				{
					UnlockAll(gb);
					HandleReply(gb, GCodeResult::ok, "");
					CheckForDeferredPause(gb);
				}
			}
			return true;
		}
#endif
	}
	return false;
}

// Restore positions etc. when exiting simulation mode
void GCodes::EndSimulation(GCodeBuffer *gb) noexcept
{
	// Ending a simulation, so restore the position
	RestorePosition(simulationRestorePoint, gb);
	reprap.SelectTool(simulationRestorePoint.toolNumber, true);
	ToolOffsetTransform(moveState.currentUserPosition, moveState.coords);
	reprap.GetMove().SetNewPosition(moveState.coords, true);
	axesVirtuallyHomed = axesHomed;
	reprap.MoveUpdated();
}

// Check for and execute triggers
void GCodes::CheckTriggers() noexcept
{
	for (unsigned int i = 0; i < MaxTriggers; ++i)
	{
		if (!triggersPending.IsBitSet(i) && triggers[i].Check())
		{
			triggersPending.SetBit(i);
		}
	}

	// If any triggers are pending, activate the one with the lowest number
	if (triggersPending.IsNonEmpty())
	{
		const unsigned int lowestTriggerPending = triggersPending.LowestSetBit();
		if (lowestTriggerPending == 0)
		{
			triggersPending.ClearBit(lowestTriggerPending);			// clear the trigger
			DoEmergencyStop();
		}
		else if (!IsTriggerBusy() && triggerGCode->GetState() == GCodeState::normal)	// if we are not already executing a trigger or config.g
		{
			if (lowestTriggerPending == 1)
			{
				if (!IsReallyPrinting())
				{
					triggersPending.ClearBit(lowestTriggerPending);	// ignore a pause trigger if we are already paused or not printing
				}
				else if (LockMovement(*triggerGCode))				// need to lock movement before executing the pause macro
				{
					triggersPending.ClearBit(lowestTriggerPending);	// clear the trigger
					DoPause(*triggerGCode, PrintPausedReason::trigger, GCodeState::pausing1);
					platform.SendAlert(GenericMessage, "Print paused by external trigger", "Printing paused", 1, 0.0, AxesBitmap());
				}
			}
			else
			{
				triggersPending.ClearBit(lowestTriggerPending);		// clear the trigger
				String<StringLength20> filename;
				filename.printf("trigger%u.g", lowestTriggerPending);
				DoFileMacro(*triggerGCode, filename.c_str(), true, AsyncSystemMacroCode);
			}
		}
	}
}

// Execute an emergency stop
void GCodes::DoEmergencyStop() noexcept
{
	reprap.EmergencyStop();
	Reset();
	platform.Message(GenericMessage, "Emergency Stop! Reset the controller to continue.\n");
}

// Pause the print
// Before calling this, check that we are doing a file print that isn't already paused and get the movement lock.
void GCodes::DoPause(GCodeBuffer& gb, PrintPausedReason reason, GCodeState newState) noexcept
{
	pausedInMacro = false;
	if (&gb == fileGCode)
	{
		// Pausing a file print because of a command in the file itself
		SavePosition(pauseRestorePoint, gb);
	}
	else
	{
		// Pausing a file print via another input source or for some other reason
		pauseRestorePoint.feedRate = fileGCode->LatestMachineState().feedRate;				// set up the default

		const bool movesSkipped = reprap.GetMove().PausePrint(pauseRestorePoint);			// tell Move we wish to pause the current print
		if (movesSkipped)
		{
			// The PausePrint call has filled in the restore point with machine coordinates
			ToolOffsetInverseTransform(pauseRestorePoint.moveCoords, moveState.currentUserPosition);	// transform the returned coordinates to user coordinates
			ClearMove();
		}
		else if (moveState.segmentsLeft != 0)
		{
			// We were not able to skip any moves, however we can skip the move that is waiting
			pauseRestorePoint.virtualExtruderPosition = moveState.virtualExtruderPosition;
			pauseRestorePoint.filePos = moveState.filePos;
			pauseRestorePoint.feedRate = moveState.feedRate;
			pauseRestorePoint.proportionDone = moveState.GetProportionDone();
			pauseRestorePoint.initialUserC0 = moveState.initialUserC0;
			pauseRestorePoint.initialUserC1 = moveState.initialUserC1;
			ToolOffsetInverseTransform(pauseRestorePoint.moveCoords, moveState.currentUserPosition);	// transform the returned coordinates to user coordinates
			ClearMove();
		}
		else
		{
			// We were not able to skip any moves, and there is no move waiting
			pauseRestorePoint.feedRate = fileGCode->LatestMachineState().feedRate;
			pauseRestorePoint.virtualExtruderPosition = virtualExtruderPosition;
			pauseRestorePoint.proportionDone = 0.0;

			// TODO: when using RTOS there is a possible race condition in the following,
			// because we might try to pause when a waiting move has just been added but before the gcode buffer has been re-initialised ready for the next command
			pauseRestorePoint.filePos = GetFilePosition(true);
			while (fileGCode->IsDoingFileMacro())						// must call this after GetFilePosition because this changes IsDoingFileMacro
			{
				pausedInMacro = true;
				fileGCode->PopState(false);
			}
#if SUPPORT_LASER || SUPPORT_IOBITS
			pauseRestorePoint.laserPwmOrIoBits = moveState.laserPwmOrIoBits;
#endif
		}

		// Replace the paused machine coordinates by user coordinates, which we updated earlier if they were returned by Move::PausePrint
		for (size_t axis = 0; axis < numVisibleAxes; ++axis)
		{
			pauseRestorePoint.moveCoords[axis] = moveState.currentUserPosition[axis];
		}

#if HAS_SBC_INTERFACE
		if (reprap.UsingSbcInterface())
		{
			fileGCode->Init();															// clear the next move
			UnlockAll(*fileGCode);														// release any locks it had
		}
		else
#endif
		{
#if HAS_MASS_STORAGE
			// If we skipped any moves, reset the file pointer to the start of the first move we need to replay
			// The following could be delayed until we resume the print
			if (pauseRestorePoint.filePos != noFilePosition)
			{
				FileData& fdata = fileGCode->LatestMachineState().fileState;
				if (fdata.IsLive())
				{
					fileGCode->RestartFrom(pauseRestorePoint.filePos);					// TODO we ought to restore the line number too, but currently we don't save it
					UnlockAll(*fileGCode);												// release any locks it had
				}
			}
#endif
		}

		codeQueue->PurgeEntries();

		if (reprap.Debug(moduleGcodes))
		{
			platform.MessageF(GenericMessage, "Paused print, file offset=%" PRIu32 "\n", pauseRestorePoint.filePos);
		}
	}

#if SUPPORT_LASER
	if (machineType == MachineType::laser)
	{
		moveState.laserPwmOrIoBits.laserPwm = 0;										// turn off the laser when we start moving
	}
#endif

	pauseRestorePoint.toolNumber = reprap.GetCurrentToolNumber();
	pauseRestorePoint.fanSpeed = lastDefaultFanSpeed;

#if HAS_MASS_STORAGE || HAS_SBC_INTERFACE
	if (!IsSimulating())
	{
		SaveResumeInfo(false);															// create the resume file so that we can resume after power down
	}
#endif

	gb.SetState(newState);
	pauseState = PauseState::pausing;

#if HAS_SBC_INTERFACE
	if (reprap.UsingSbcInterface())
	{
		// Prepare notification for the SBC
		reprap.GetSbcInterface().SetPauseReason(pauseRestorePoint.filePos, reason);
	}
#endif

	if (pauseRestorePoint.filePos == noFilePosition)
	{
		// Make sure we expose usable values (which noFilePosition is not)
		pauseRestorePoint.filePos = 0;
	}

	reprap.StateUpdated();																// test DWC/DSF that we have changed a restore point
}

// Check if a pause is pending, action it if so
void GCodes::CheckForDeferredPause(GCodeBuffer& gb) noexcept
{
	if (&gb == fileGCode && !gb.IsDoingFileMacro() && deferredPauseCommandPending != nullptr)
	{
		gb.PutAndDecode(deferredPauseCommandPending);
		deferredPauseCommandPending = nullptr;
	}
}

// Return true if we are printing from SD card and not pausing, paused or resuming
// Called by the filament monitor code to see whether we need to activate the filament monitors, and from other places
// TODO make this independent of PrintMonitor
bool GCodes::IsReallyPrinting() const noexcept
{
#if SUPPORT_REMOTE_COMMANDS
	if (CanInterface::InExpansionMode())
	{
		return isRemotePrinting;
	}
#endif

	return reprap.GetPrintMonitor().IsPrinting() && pauseState == PauseState::notPaused;
}

bool GCodes::IsReallyPrintingOrResuming() const noexcept
{
	return reprap.GetPrintMonitor().IsPrinting() && (pauseState == PauseState::notPaused || pauseState == PauseState::resuming);
}

// Return true if the SD card print is waiting for a heater to reach temperature
bool GCodes::IsHeatingUp() const noexcept
{
	int num;
	return fileGCode->IsExecuting()
		&& fileGCode->GetCommandLetter() == 'M'
		&& ((num = fileGCode->GetCommandNumber()) == 109 || num == 116 || num == 190 || num == 191);
}

#if HAS_VOLTAGE_MONITOR || HAS_STALL_DETECT

// Do an emergency pause following loss of power or a motor stall returning true if successful, false if needs to be retried
bool GCodes::DoEmergencyPause() noexcept
{
	if (!autoPauseGCode->IsCompletelyIdle())
	{
		return false;							// we can't pause if the auto pause thread is busy already
	}

	// Save the resume info, stop movement immediately and run the low voltage pause script to lift the nozzle etc.
	GrabMovement(*autoPauseGCode);

	// When we use RTOS there is a possible race condition in the following, because we might try to pause when a waiting move has just been added
	// but before the gcode buffer has been re-initialised ready for the next command. So start a critical section.
	TaskCriticalSectionLocker lock;

	const bool movesSkipped = reprap.GetMove().LowPowerOrStallPause(pauseRestorePoint);
	if (movesSkipped)
	{
		// The PausePrint call has filled in the restore point with machine coordinates
		ToolOffsetInverseTransform(pauseRestorePoint.moveCoords, moveState.currentUserPosition);	// transform the returned coordinates to user coordinates
		ClearMove();
	}
	else if (moveState.segmentsLeft != 0 && moveState.filePos != noFilePosition)
	{
		// We were not able to skip any moves, however we can skip the remaining segments of this current move
		ToolOffsetInverseTransform(moveState.initialCoords, moveState.currentUserPosition);
		pauseRestorePoint.feedRate = moveState.feedRate;
		pauseRestorePoint.virtualExtruderPosition = moveState.virtualExtruderPosition;
		pauseRestorePoint.filePos = moveState.filePos;
		pauseRestorePoint.proportionDone = moveState.GetProportionDone();
		pauseRestorePoint.initialUserC0 = moveState.initialUserC0;
		pauseRestorePoint.initialUserC1 = moveState.initialUserC1;
#if SUPPORT_LASER || SUPPORT_IOBITS
		pauseRestorePoint.laserPwmOrIoBits = moveState.laserPwmOrIoBits;
#endif
		ClearMove();
	}
	else
	{
		// We were not able to skip any moves, and if there is a move waiting then we can't skip that one either
		pauseRestorePoint.feedRate = fileGCode->LatestMachineState().feedRate;
		pauseRestorePoint.virtualExtruderPosition = virtualExtruderPosition;

		pauseRestorePoint.filePos = GetFilePosition(true);
		pauseRestorePoint.proportionDone = 0.0;

#if SUPPORT_LASER || SUPPORT_IOBITS
		pauseRestorePoint.laserPwmOrIoBits = moveState.laserPwmOrIoBits;
#endif
	}

#if HAS_SBC_INTERFACE
	if (reprap.UsingSbcInterface())
	{
		PrintPausedReason reason = platform.IsPowerOk() ? PrintPausedReason::stall : PrintPausedReason::lowVoltage;
		reprap.GetSbcInterface().SetEmergencyPauseReason(pauseRestorePoint.filePos, reason);
	}
#endif

	codeQueue->PurgeEntries();

	// Replace the paused machine coordinates by user coordinates, which we updated earlier
	for (size_t axis = 0; axis < numVisibleAxes; ++axis)
	{
		pauseRestorePoint.moveCoords[axis] = moveState.currentUserPosition[axis];
	}

	if (pauseRestorePoint.filePos == noFilePosition)
	{
		// Make sure we expose usable values (which noFilePosition is not)
		pauseRestorePoint.filePos = 0;
	}
	pauseRestorePoint.toolNumber = reprap.GetCurrentToolNumber();
	pauseRestorePoint.fanSpeed = lastDefaultFanSpeed;
	pauseState = PauseState::paused;

	return true;
}

#endif

#if HAS_VOLTAGE_MONITOR

// Try to pause the current SD card print, returning true if successful, false if needs to be called again
bool GCodes::LowVoltagePause() noexcept
{
	if (IsSimulating())
	{
		return true;								// ignore the low voltage indication
	}

	reprap.GetHeat().SuspendHeaters(true);			// turn the heaters off to conserve power for the motors to execute the pause
	switch (pauseState)
	{
	case PauseState::resuming:
		// This is an unlucky situation, because the resume macro is probably being run, which will probably lower the head back on to the print.
		// It may well be that the power loss will prevent the resume macro being completed. If not, try again when the print has been resumed.
		return false;

	case PauseState::pausing:
		// We are in the process of pausing already, so the resume info has already been saved.
		// With luck the retraction and lifting of the head in the pause.g file has been done already.
		return true;

	case PauseState::paused:
		// Resume info has already been saved, and resuming will be prevented while the power is low
		return true;

	default:
		break;
	}

	if (reprap.GetPrintMonitor().IsPrinting())
	{
		if (!DoEmergencyPause())
		{
			return false;
		}

		// Run the auto-pause script
		if (powerFailScript != nullptr)
		{
			autoPauseGCode->PutAndDecode(powerFailScript);
		}
		autoPauseGCode->SetState(GCodeState::powerFailPausing1);
		isPowerFailPaused = true;

		// Don't do any more here, we want the auto pause thread to run as soon as possible
	}

	return true;
}

// Resume printing, normally only ever called after it has been paused because if low voltage.
// If the pause was short enough, resume automatically.
bool GCodes::LowVoltageResume() noexcept
{
	reprap.GetHeat().SuspendHeaters(false);			// turn the heaters on again
	if (pauseState != PauseState::notPaused && isPowerFailPaused)
	{
		isPowerFailPaused = false;					// pretend it's a normal pause
		// Run resurrect.g automatically
		//TODO qq;
		//platform.Message(LoggedGenericMessage, "Print auto-resumed\n");
	}
	return true;
}

#endif

#if HAS_MASS_STORAGE || HAS_SBC_INTERFACE

void GCodes::SaveResumeInfo(bool wasPowerFailure) noexcept
{
	const char* const printingFilename = reprap.GetPrintMonitor().GetPrintingFilename();
	if (printingFilename != nullptr)
	{
		FileStore * const f = platform.OpenSysFile(RESUME_AFTER_POWER_FAIL_G, OpenMode::write);
		if (f == nullptr)
		{
			platform.MessageF(ErrorMessage, "Failed to create file %s\n", RESUME_AFTER_POWER_FAIL_G);
		}
		else
		{
			String<StringLength256> buf;

			// Write the header comment
			buf.printf("; File \"%s\" resume print after %s", printingFilename, (wasPowerFailure) ? "power failure" : "print paused");
			tm timeInfo;
			if (platform.GetDateTime(timeInfo))
			{
				buf.catf(" at %04u-%02u-%02u %02u:%02u",
								timeInfo.tm_year + 1900, timeInfo.tm_mon + 1, timeInfo.tm_mday, timeInfo.tm_hour, timeInfo.tm_min);
			}
			buf.cat("\nG21\n");												// set units to mm because we will be writing positions in mm
			bool ok = f->Write(buf.c_str())
					&& reprap.GetHeat().WriteBedAndChamberTempSettings(f)	// turn on bed and chamber heaters
					&& reprap.GetMove().WriteResumeSettings(f);				// load grid, if we are using one
			if (ok)
			{
				// Write a G92 command to say where the head is. This is useful if we can't Z-home the printer with a print on the bed and the Z steps/mm is high.
				// The paused coordinates include any tool offsets and baby step offsets, so remove those.
				// We used to send T-1 here ensure that no tool is selected, in case config.g selects one and it has an offset.
				// We no longer do that because on some tool changers it is possible to home X and Y with a tool loaded.
				buf.copy("G92");
				for (size_t axis = 0; axis < numVisibleAxes; ++axis)
				{
					const float totalOffset = currentBabyStepOffsets[axis] - GetCurrentToolOffset(axis);
					buf.catf(" %c%.3f", axisLetters[axis], (double)(pauseRestorePoint.moveCoords[axis] - totalOffset));
				}
				buf.cat("\nG60 S1\n");										// save the coordinates as restore point 1 too
				ok = f->Write(buf.c_str());
			}
			if (ok)
			{
				ok = reprap.WriteToolSettings(f);							// set tool temperatures, tool mix ratios etc. and select the current tool without running tool change files
			}
			if (ok)
			{
				buf.printf("M98 P\"%s\"\n", RESUME_PROLOGUE_G);				// call the prologue
				ok = f->Write(buf.c_str());
			}
			if (ok)
			{
				buf.copy("M116\nM290");										// wait for temperatures and start writing baby stepping offsets
				for (size_t axis = 0; axis < numVisibleAxes; ++axis)
				{
					buf.catf(" %c%.3f", axisLetters[axis], (double)GetTotalBabyStepOffset(axis));
				}
				buf.cat(" R0\n");
				ok = f->Write(buf.c_str());									// write baby stepping offsets
			}

			// Now that we have homed, we can run the tool change files for the current tool
			const Tool * const ct = reprap.GetCurrentTool();
			if (ok && ct != nullptr)
			{
				buf.printf("T-1 P0\nT%u P6\n", ct->Number());				// deselect the current tool without running tfree, and select it running tpre and tpost
				ok = f->Write(buf.c_str());									// write tool selection
			}

#if SUPPORT_WORKPLACE_COORDINATES
			// Restore the coordinate offsets of all workplaces
			if (ok)
			{
				ok = WriteWorkplaceCoordinates(f);
			}

			if (ok)
			{
				// Switch to the correct workplace. 'currentCoordinateSystem' is 0-based.
				if (moveState.currentCoordinateSystem <= 5)
				{
					buf.printf("G%u\n", 54 + moveState.currentCoordinateSystem);
				}
				else
				{
					buf.printf("G59.%u\n", moveState.currentCoordinateSystem - 5);
				}
				ok = f->Write(buf.c_str());
			}
#else
			if (ok)
			{
				buf.copy("M206");
				for (size_t axis = 0; axis < numVisibleAxes; ++axis)
				{
					buf.catf(" %c%.3f", axisLetters[axis], (double)-workplaceCoordinates[0][axis]);
				}
				buf.cat('\n');
				ok = f->Write(buf.c_str());
			}
#endif

			if (ok && fileGCode->OriginalMachineState().volumetricExtrusion)
			{
				buf.copy("M200 ");
				char c = 'D';
				for (size_t i = 0; i < numExtruders; ++i)
				{
					buf.catf("%c%.03f", c, (double)volumetricExtrusionFactors[i]);
					c = ':';
				}
				buf.cat('\n');
				ok = f->Write(buf.c_str());									// write volumetric extrusion factors
			}
			if (ok)
			{
				buf.printf("M106 S%.2f\n", (double)lastDefaultFanSpeed);
				ok = f->Write(buf.c_str())									// set the speed of the print fan after we have selected the tool
					&& reprap.GetFansManager().WriteFanSettings(f);			// set the speeds of all non-thermostatic fans after setting the default fan speed
			}
			if (ok)
			{
				buf.printf("M116\nG92 E%.5f\n%s\n", (double)virtualExtruderPosition, (fileGCode->OriginalMachineState().drivesRelative) ? "M83" : "M82");
				ok = f->Write(buf.c_str());									// write virtual extruder position and absolute/relative extrusion flag
			}
			if (ok)
			{
				ok = buildObjects.WriteObjectDirectory(f);					// write the state of printing objects
			}
			if (ok)
			{
				const unsigned int selectedPlane = fileGCode->OriginalMachineState().selectedPlane;
				buf.printf("G%u\nM23 \"%s\"\nM26 S%" PRIu32, selectedPlane + 17, printingFilename, pauseRestorePoint.filePos);
				if (pauseRestorePoint.proportionDone > 0.0)
				{
					buf.catf(" P%.3f %c%.3f %c%.3f",
							(double)pauseRestorePoint.proportionDone,
							(selectedPlane == 2) ? 'Y' : 'X', (double)pauseRestorePoint.initialUserC0,
							(selectedPlane == 0) ? 'Y' : 'Z', (double)pauseRestorePoint.initialUserC1);
				}
				buf.cat('\n');
				ok = f->Write(buf.c_str());									// write G17/18/19, filename and file position, and if necessary proportion done and initial XY position
			}
			if (ok)
			{
				// Build the commands to restore the head position. These assume that we are working in mm.
				// Start with a vertical move to 2mm above the final Z position
				buf.printf("G0 F6000 Z%.3f\n", (double)(pauseRestorePoint.moveCoords[Z_AXIS] + 2.0));

				// Now set all the other axes
				buf.cat("G0 F6000");
				for (size_t axis = 0; axis < numVisibleAxes; ++axis)
				{
					if (axis != Z_AXIS)
					{
						buf.catf(" %c%.3f", axisLetters[axis], (double)pauseRestorePoint.moveCoords[axis]);
					}
				}

				// Now move down to the correct Z height
				buf.catf("\nG0 F6000 Z%.3f\n", (double)pauseRestorePoint.moveCoords[Z_AXIS]);

				// Set the feed rate
				buf.catf("G1 F%.1f", (double)InverseConvertSpeedToMmPerMin(pauseRestorePoint.feedRate));
#if SUPPORT_LASER
				if (machineType == MachineType::laser)
				{
					buf.catf(" S%u", (unsigned int)pauseRestorePoint.laserPwmOrIoBits.laserPwm);
				}
				else
				{
#endif
#if SUPPORT_IOBITS
					buf.catf(" P%u", (unsigned int)pauseRestorePoint.laserPwmOrIoBits.ioBits);
#endif
#if SUPPORT_LASER
				}
#endif
				buf.cat("\n");
				ok = f->Write(buf.c_str());									// restore feed rate and output bits or laser power
			}
			if (ok)
			{
				buf.printf("%s\nM24\n", (fileGCode->OriginalMachineState().usingInches) ? "G20" : "G21");
				ok = f->Write(buf.c_str());									// restore inches/mm and resume printing
			}
			if (!f->Close())
			{
				ok = false;
			}
			if (ok)
			{
				platform.Message(LoggedGenericMessage, "Resume state saved\n");
			}
			else
			{
				platform.DeleteSysFile(RESUME_AFTER_POWER_FAIL_G);
				platform.MessageF(ErrorMessage, "Failed to write or close file %s\n", RESUME_AFTER_POWER_FAIL_G);
			}
		}
	}
}

#endif

void GCodes::Diagnostics(MessageType mtype) noexcept
{
	platform.Message(mtype, "=== GCodes ===\n");
	platform.MessageF(mtype, "Segments left: %u\n", moveState.segmentsLeft);
	const GCodeBuffer * const movementOwner = resourceOwners[MoveResource];
	platform.MessageF(mtype, "Movement lock held by %s\n", (movementOwner == nullptr) ? "null" : movementOwner->GetChannel().ToString());

	for (GCodeBuffer *gb : gcodeSources)
	{
		if (gb != nullptr)
		{
			gb->Diagnostics(mtype);
		}
	}

	codeQueue->Diagnostics(mtype);
}

// Lock movement and wait for pending moves to finish.
// As a side-effect it loads moveBuffer with the last position and feedrate for you.
bool GCodes::LockMovementAndWaitForStandstill(GCodeBuffer& gb) noexcept
{
	// Lock movement to stop another source adding moves to the queue
	if (!LockMovement(gb))
	{
		return false;
	}

	// Last one gone?
	if (moveState.segmentsLeft != 0)
	{
		return false;
	}

	// Wait for all the queued moves to stop so we get the actual last position
	if (!reprap.GetMove().WaitingForAllMovesFinished())
	{
		return false;
	}

	if (&gb != queuedGCode && !IsCodeQueueIdle())		// wait for deferred command queue to catch up
	{
		return false;
	}

	gb.MotionStopped();									// must do this after we have finished waiting, so that we don't stop waiting when executing G4

	if (RTOSIface::GetCurrentTask() == Tasks::GetMainTask())
	{
		// Get the current positions. These may not be the same as the ones we remembered from last time if we just did a special move.
		UpdateCurrentUserPosition(gb);
	}
	else
	{
		// Cannot update the user position from external tasks. Do it later
		updateUserPositionGb = &gb;
	}
	return true;
}

// Save (some of) the state of the machine for recovery in the future.
bool GCodes::Push(GCodeBuffer& gb, bool withinSameFile) noexcept
{
	const bool ok = gb.PushState(withinSameFile);
	if (!ok)
	{
		platform.Message(ErrorMessage, "Push(): stack overflow\n");
		AbortPrint(gb);
	}
	return ok;
}

// Recover a saved state
void GCodes::Pop(GCodeBuffer& gb, bool withinSameFile) noexcept
{
	if (!gb.PopState(withinSameFile))
	{
		platform.Message(ErrorMessage, "Pop(): stack underflow\n");
	}
	reprap.InputsUpdated();
}

// Set up the extrusion and feed rate of a move for the Move class
// 'moveBuffer.moveType' and 'moveBuffer.isCoordinated' must be set up before calling this
// 'isPrintingMove' is true if there is any axis movement
// Returns nullptr if this gcode is valid so far, or an error message if it should be discarded
const char * GCodes::LoadExtrusionAndFeedrateFromGCode(GCodeBuffer& gb, bool isPrintingMove) THROWS(GCodeException)
{
	// Deal with feed rate, also determine whether M220 and M221 speed and extrusion factors apply to this move
	if (moveState.isCoordinated || machineType == MachineType::fff)
	{
		moveState.applyM220M221 = (moveState.moveType == 0 && isPrintingMove && !gb.IsDoingFileMacro());
		if (gb.Seen(feedrateLetter))
		{
			gb.LatestMachineState().feedRate = gb.GetSpeed();				// update requested speed, not allowing for speed factor
		}
		moveState.feedRate = (moveState.applyM220M221)
								? speedFactor * gb.LatestMachineState().feedRate
								: gb.LatestMachineState().feedRate;
		moveState.usingStandardFeedrate = true;
	}
	else
	{
		moveState.applyM220M221 = false;
		moveState.feedRate = ConvertSpeedFromMmPerMin(MaximumG0FeedRate);	// use maximum feed rate, the M203 parameters will limit it
		moveState.usingStandardFeedrate = false;
	}

	// Zero every extruder drive as some drives may not be moved
	for (size_t drive = numTotalAxes; drive < MaxAxesPlusExtruders; drive++)
	{
		moveState.coords[drive] = 0.0;
	}
	moveState.hasPositiveExtrusion = false;
	moveState.virtualExtruderPosition = virtualExtruderPosition;			// save this before we update it
	ExtrudersBitmap extrudersMoving;

	// Check if we are extruding
	if (gb.Seen(extrudeLetter))												// DC 2018-08-07: at E3D's request, extrusion is now recognised even on uncoordinated moves
	{
		// Check that we have a tool to extrude with
		Tool* const tool = reprap.GetCurrentTool();
		if (tool == nullptr)
		{
			displayNoToolWarning = true;
			return nullptr;
		}

		const size_t eMoveCount = tool->DriveCount();
		if (eMoveCount != 0)
		{
			// Set the drive values for this tool
			float eMovement[MaxExtruders];
			size_t mc = eMoveCount;
			gb.GetFloatArray(eMovement, mc, false);

			if (mc == 1)
			{
				// There may be multiple extruders present but only one value has been specified, so use mixing
				const float moveArg = gb.ConvertDistance(eMovement[0]);
				float requestedExtrusionAmount;
				if (gb.LatestMachineState().drivesRelative)
				{
					requestedExtrusionAmount = moveArg;
				}
				else
				{
					requestedExtrusionAmount = moveArg - virtualExtruderPosition;
					virtualExtruderPosition = moveArg;
				}

				if (requestedExtrusionAmount > 0.0)
				{
					moveState.hasPositiveExtrusion = true;
				}

				// rawExtruderTotal is used to calculate print progress, so it must be based on the requested extrusion from the slicer
				// before accounting for mixing, extrusion factor etc.
				// We used to have 'isPrintingMove &&' in the condition too, but this excluded wipe-while-retracting moves, so it gave wrong results for % print complete.
				// We still exclude extrusion during tool changing and other macros, because that is extrusion not known to the slicer.
				if (moveState.moveType == 0 && !gb.IsDoingFileMacro())
				{
					rawExtruderTotal += requestedExtrusionAmount;
				}

				float totalMix = 0.0;
				for (size_t eDrive = 0; eDrive < eMoveCount; eDrive++)
				{
					const float thisMix = tool->GetMix()[eDrive];
					if (thisMix != 0.0)
					{
						totalMix += thisMix;
						const int extruder = tool->GetDrive(eDrive);
						float extrusionAmount = requestedExtrusionAmount * thisMix;
						if (gb.LatestMachineState().volumetricExtrusion)
						{
							extrusionAmount *= volumetricExtrusionFactors[extruder];
						}
						if (eDrive == 0 && moveState.moveType == 0 && !gb.IsDoingFileMacro())
						{
							rawExtruderTotalByDrive[extruder] += extrusionAmount;
						}

						moveState.coords[ExtruderToLogicalDrive(extruder)] = (moveState.applyM220M221)
																				? extrusionAmount * extrusionFactors[extruder]
																				: extrusionAmount;
						extrudersMoving.SetBit(extruder);
					}
				}
				if (!isPrintingMove && moveState.usingStandardFeedrate)
				{
					// For E3D: If the total mix ratio is greater than 1.0 then we should scale the feed rate accordingly, e.g. for dual serial extruder drives
					moveState.feedRate *= totalMix;
				}
			}
			else
			{
				// Individual extrusion amounts have been provided. This is supported in relative extrusion mode only.
				// Note, if this is an extruder-only movement then the feed rate will apply to the total of all active extruders
				if (gb.LatestMachineState().drivesRelative)
				{
					for (size_t eDrive = 0; eDrive < mc; eDrive++)
					{
						const int extruder = tool->GetDrive(eDrive);
						float extrusionAmount = gb.ConvertDistance(eMovement[eDrive]);
						if (extrusionAmount != 0.0)
						{
							if (extrusionAmount > 0.0)
							{
								moveState.hasPositiveExtrusion = true;
							}

							if (gb.LatestMachineState().volumetricExtrusion)
							{
								extrusionAmount *= volumetricExtrusionFactors[extruder];
							}

							if (eDrive < mc && moveState.moveType == 0 && !gb.IsDoingFileMacro())
							{
								rawExtruderTotalByDrive[extruder] += extrusionAmount;
								rawExtruderTotal += extrusionAmount;
							}
							moveState.coords[ExtruderToLogicalDrive(extruder)] = (moveState.applyM220M221)
																					? extrusionAmount * extrusionFactors[extruder]
																					: extrusionAmount;
							extrudersMoving.SetBit(extruder);
						}
					}
				}
				else
				{
					return "Multiple E parameters in G1 commands are not supported in absolute extrusion mode";
				}
			}
		}
	}

	if (moveState.moveType == 1 || moveState.moveType == 4)
	{
		if (!platform.GetEndstops().EnableExtruderEndstops(extrudersMoving))
		{
			return "Failed to enable extruder endstops";
		}
	}

	return nullptr;
}

// Check that enough axes have been homed, returning true if insufficient axes homed
bool GCodes::CheckEnoughAxesHomed(AxesBitmap axesMoved) noexcept
{
	return (reprap.GetMove().GetKinematics().MustBeHomedAxes(axesMoved, noMovesBeforeHoming) & ~axesVirtuallyHomed).IsNonEmpty();
}

// Execute a straight move
// If not ready, return false
// If we can't execute the move, return true with 'err' set to the error message
// Else return true with 'err' left alone (it is set to nullptr on entry)
// We have already acquired the movement lock and waited for the previous move to be taken.
bool GCodes::DoStraightMove(GCodeBuffer& gb, bool isCoordinated, const char *& err) THROWS(GCodeException)
{
	if (moveFractionToSkip > 0.0)
	{
		moveState.initialUserC0 = restartInitialUserC0;
		moveState.initialUserC1 = restartInitialUserC1;
	}
	else
	{
		const unsigned int selectedPlane = gb.LatestMachineState().selectedPlane;
		moveState.initialUserC0 = moveState.currentUserPosition[(selectedPlane == 2) ? Y_AXIS : X_AXIS];
		moveState.initialUserC1 = moveState.currentUserPosition[(selectedPlane == 0) ? Y_AXIS : Z_AXIS];
	}

	// Set up default move parameters
	moveState.isCoordinated = isCoordinated;
	moveState.checkEndstops = false;
	moveState.reduceAcceleration = false;
	moveState.moveType = 0;
	moveState.tool = reprap.GetCurrentTool();
	moveState.usePressureAdvance = false;
	axesToSenseLength.Clear();

	// Check to see if the move is a 'homing' move that endstops are checked on.
	// We handle H1 parameters affecting extrusion elsewhere.
	if (gb.Seen('H') || (machineType != MachineType::laser && gb.Seen('S')))
	{
		const int ival = gb.GetIValue();
		if (ival >= 1 && ival <= 4)
		{
			if (!LockMovementAndWaitForStandstill(gb))
			{
				return false;
			}
			moveState.moveType = ival;
			moveState.tool = nullptr;
		}
		if (!gb.Seen('H'))
		{
			platform.Message(MessageType::WarningMessage, "Obsolete use of S parameter on G1 command. Use H parameter instead.\n");
		}
	}

	// Check for 'R' parameter to move relative to a restore point
	const RestorePoint * rp = nullptr;
	if (moveState.moveType == 0 && gb.Seen('R'))
	{
		const uint32_t rParam = gb.GetUIValue();
		if (rParam < ARRAY_SIZE(numberedRestorePoints))
		{
			rp = &numberedRestorePoints[rParam];
		}
		else
		{
			err = "G0/G1: bad restore point number";
			return true;
		}
	}

	// Check for laser power setting or IOBITS
#if SUPPORT_LASER || SUPPORT_IOBITS
	if (rp != nullptr)
	{
		moveState.laserPwmOrIoBits = rp->laserPwmOrIoBits;
	}
# if SUPPORT_LASER
	else if (machineType == MachineType::laser)
	{
		if (gb.Seen('S'))
		{
			moveState.laserPwmOrIoBits.laserPwm = ConvertLaserPwm(gb.GetFValue());
		}
		else if (moveState.moveType != 0)
		{
			moveState.laserPwmOrIoBits.laserPwm = 0;			// it's a homing move or similar, so turn the laser off
		}
		else if (laserPowerSticky)
		{
			// Leave the laser PWM alone because this is what LaserWeb expects. If it is an uncoordinated move then the motion system will turn the laser off.
			// This means that after a G0 move, the next G1 move will default to the same power as the previous G1 move, as LightBurn expects.
		}
		else
		{
			moveState.laserPwmOrIoBits.laserPwm = 0;
		}
	}
# endif
# if SUPPORT_IOBITS
	else
	{
		// Update the iobits parameter
		if (gb.Seen('P'))
		{
			moveState.laserPwmOrIoBits.ioBits = (IoBits_t)gb.GetIValue();
		}
		else
		{
			// Leave moveBuffer.ioBits alone so that we keep the previous value
		}
	}
# endif
#endif

	if (moveState.moveType != 0)
	{
		// This may be a raw motor move, in which case we need the current raw motor positions in moveBuffer.coords.
		// If it isn't a raw motor move, it will still be applied without axis or bed transform applied,
		// so make sure the initial coordinates don't have those either to avoid unwanted Z movement.
		reprap.GetMove().GetCurrentUserPosition(moveState.coords, moveState.moveType, reprap.GetCurrentTool());
	}

	// Set up the initial coordinates
	memcpyf(moveState.initialCoords, moveState.coords, numVisibleAxes);

	// Save the current position, we need it possibly later
	float initialUserPosition[MaxAxes];
	memcpyf(initialUserPosition, moveState.currentUserPosition, numVisibleAxes);

	AxesBitmap axesMentioned;
	for (size_t axis = 0; axis < numVisibleAxes; axis++)
	{
		if (gb.Seen(axisLetters[axis]))
		{
			// If it is a special move on a delta, movement must be relative.
			if (moveState.moveType != 0 && !gb.LatestMachineState().axesRelative && reprap.GetMove().GetKinematics().GetKinematicsType() == KinematicsType::linearDelta)
			{
				err = "G0/G1: attempt to move individual motors of a delta machine to absolute positions";
				return true;
			}

			axesMentioned.SetBit(axis);
			const float moveArg = gb.GetDistance();
			if (moveState.moveType != 0)
			{
				// Special moves update the move buffer directly, bypassing the user coordinates
				if (gb.LatestMachineState().axesRelative)
				{
					moveState.coords[axis] += moveArg * (1.0 - moveFractionToSkip);
				}
				else
				{
					moveState.coords[axis] = moveArg;
				}
			}
			else if (rp != nullptr)
			{
				moveState.currentUserPosition[axis] = moveArg + rp->moveCoords[axis];
				// When a restore point is being used (G1 R parameter) then we used to set any coordinates that were not mentioned to the restore point values.
				// But that causes issues for tool change on IDEX machines because we end up restoring the U axis when we shouldn't.
				// So we no longer do that, and the user must mention any axes that he wants restored e.g. G1 R2 X0 Y0.
			}
			else if (gb.LatestMachineState().axesRelative)
			{
				moveState.currentUserPosition[axis] += moveArg * (1.0 - moveFractionToSkip);
			}
			else if (gb.LatestMachineState().g53Active)
			{
				moveState.currentUserPosition[axis] = moveArg + GetCurrentToolOffset(axis);	// g53 ignores tool offsets as well as workplace coordinates
			}
			else if (gb.LatestMachineState().runningSystemMacro)
			{
				moveState.currentUserPosition[axis] = moveArg;								// don't apply workplace offsets to commands in system macros
			}
			else
			{
				moveState.currentUserPosition[axis] = moveArg + GetWorkplaceOffset(axis);
			}
		}
	}

	// Check enough axes have been homed
	switch (moveState.moveType)
	{
	case 0:
		if (!doingManualBedProbe && CheckEnoughAxesHomed(axesMentioned))
		{
			err = "G0/G1: insufficient axes homed";
			return true;
		}
		break;

	case 3:
		axesToSenseLength = axesMentioned & AxesBitmap::MakeLowestNBits(numTotalAxes);
		// no break
	case 1:
	case 4:
		{
			bool reduceAcceleration;
			if (!platform.GetEndstops().EnableAxisEndstops(axesMentioned & AxesBitmap::MakeLowestNBits(numTotalAxes), moveState.moveType == 1, reduceAcceleration))
			{
				err = "Failed to enable endstops";
				return true;
			}
			moveState.reduceAcceleration = reduceAcceleration;
		}
		moveState.checkEndstops = true;
		break;

	case 2:
	default:
		break;
	}

	err = LoadExtrusionAndFeedrateFromGCode(gb, axesMentioned.IsNonEmpty());	// for type 1 moves, this must be called after calling EnableAxisEndstops, because EnableExtruderEndstop assumes that
	if (err != nullptr)
	{
		return true;
	}

	const bool isPrintingMove = moveState.hasPositiveExtrusion && axesMentioned.IsNonEmpty();
	if (buildObjects.IsFirstMoveSincePrintingResumed())							// if this is the first move after skipping an object
	{
		if (isPrintingMove)
		{
			if (TravelToStartPoint(gb))											// don't start a printing move from the wrong place
			{
				buildObjects.DoneMoveSincePrintingResumed();
			}
			return false;
		}
		else if (axesMentioned.IsNonEmpty())									// don't count G1 Fxxx as a travel move
		{
			buildObjects.DoneMoveSincePrintingResumed();
		}
	}

#if TRACK_OBJECT_NAMES
	if (isPrintingMove)
	{
		// Update the object coordinates limits. For efficiency, we only update the final coordinate.
		// Except in the case of a straight line that is only one extrusion width wide, this is sufficient.
		buildObjects.UpdateObjectCoordinates(moveState.currentUserPosition, axesMentioned);
	}
#endif

	// Set up the move. We must assign segmentsLeft last, so that when Move runs as a separate task the move won't be picked up by the Move process before it is complete.
	// Note that if this is an extruder-only move, we don't do axis movements to allow for tool offset changes, we defer those until an axis moves.
	if (moveState.moveType != 0)
	{
		// It's a raw motor move, so do it in a single segment and wait for it to complete
		moveState.totalSegments = 1;
		gb.SetState(GCodeState::waitingForSpecialMoveToComplete);
	}
	else if (axesMentioned.IsEmpty())
	{
		moveState.totalSegments = 1;
	}
	else
	{
#if SUPPORT_COORDINATE_ROTATION
		if (g68Angle != 0.0 && gb.DoingCoordinateRotation())
		{
			float coords[MaxAxes];
			memcpyf(coords, moveState.currentUserPosition, MaxAxes);
			RotateCoordinates(g68Angle, coords);
			ToolOffsetTransform(coords, moveState.coords, axesMentioned);
		}
		else
#endif
		{
			ToolOffsetTransform(moveState.currentUserPosition, moveState.coords, axesMentioned);	// apply tool offset, baby stepping, Z hop and axis scaling
		}

		AxesBitmap effectiveAxesHomed = axesVirtuallyHomed;
		if (doingManualBedProbe)
		{
			effectiveAxesHomed.ClearBit(Z_AXIS);							// if doing a manual Z probe, don't limit the Z movement
		}

		const LimitPositionResult lp = reprap.GetMove().GetKinematics().LimitPosition(moveState.coords, moveState.initialCoords, numVisibleAxes, effectiveAxesHomed, moveState.isCoordinated, limitAxes);
		switch (lp)
		{
		case LimitPositionResult::adjusted:
		case LimitPositionResult::adjustedAndIntermediateUnreachable:
			if (machineType != MachineType::fff)
			{
				err = "G0/G1: target position outside machine limits";		// it's a laser or CNC so this is a definite error
				return true;
			}
			ToolOffsetInverseTransform(moveState.coords, moveState.currentUserPosition);	// make sure the limits are reflected in the user position
			if (lp == LimitPositionResult::adjusted)
			{
				break;														// we can reach the intermediate positions, so nothing more to do
			}
			// no break

		case LimitPositionResult::intermediateUnreachable:
			if (   moveState.isCoordinated
				&& (   (machineType == MachineType::fff && !moveState.hasPositiveExtrusion)
#if SUPPORT_LASER || SUPPORT_IOBITS
					|| (machineType == MachineType::laser && moveState.laserPwmOrIoBits.laserPwm == 0)
#endif
				   )
			   )
			{
				// It's a coordinated travel move on a 3D printer or laser cutter, so see whether an uncoordinated move will work
				const LimitPositionResult lp2 = reprap.GetMove().GetKinematics().LimitPosition(moveState.coords, moveState.initialCoords, numVisibleAxes, effectiveAxesHomed, false, limitAxes);
				if (lp2 == LimitPositionResult::ok)
				{
					moveState.isCoordinated = false;						// change it to an uncoordinated move
					break;
				}
			}
			err = "G0/G1: target position not reachable from current position";		// we can't bring the move within limits, so this is a definite error
			return true;

		case LimitPositionResult::ok:
		default:
			break;
		}

		// If we are emulating Marlin for nanoDLP then we need to set a special end state
		if (gb.LatestMachineState().compatibility == Compatibility::NanoDLP && !DoingFileMacro())
		{
			gb.SetState(GCodeState::waitingForSpecialMoveToComplete);
		}

		// Flag whether we should use pressure advance, if there is any extrusion in this move.
		// We assume it is a normal printing move needing pressure advance if there is forward extrusion and XYU.. movement.
		// The movement code will only apply pressure advance if there is forward extrusion, so we only need to check for XYU.. movement here.
		{
			AxesBitmap axesMentionedExceptZ = axesMentioned;
			axesMentionedExceptZ.ClearBit(Z_AXIS);
			moveState.usePressureAdvance = moveState.hasPositiveExtrusion && axesMentionedExceptZ.IsNonEmpty();
		}

		// Apply segmentation if necessary. To speed up simulation on SCARA printers, we don't apply kinematics segmentation when simulating.
		// As soon as we set segmentsLeft nonzero, the Move process will assume that the move is ready to take, so this must be the last thing we do.
		const Kinematics& kin = reprap.GetMove().GetKinematics();
		const SegmentationType st = kin.GetSegmentationType();
		if (st.useSegmentation && simulationMode != SimulationMode::normal && (moveState.hasPositiveExtrusion || moveState.isCoordinated || st.useG0Segmentation))
		{
			// This kinematics approximates linear motion by means of segmentation
			float moveLengthSquared = fsquare(moveState.currentUserPosition[X_AXIS] - initialUserPosition[X_AXIS]) + fsquare(moveState.currentUserPosition[Y_AXIS] - initialUserPosition[Y_AXIS]);
			if (st.useZSegmentation)
			{
				moveLengthSquared += fsquare(moveState.currentUserPosition[Z_AXIS] - initialUserPosition[Z_AXIS]);
			}
			const float moveLength = fastSqrtf(moveLengthSquared);
			const float moveTime = moveLength/(moveState.feedRate * StepClockRate);		// this is a best-case time, often the move will take longer
			moveState.totalSegments = (unsigned int)max<long>(1, lrintf(min<float>(moveLength * kin.GetReciprocalMinSegmentLength(), moveTime * kin.GetSegmentsPerSecond())));
		}
		else
		{
			moveState.totalSegments = 1;
		}
		if (reprap.GetMove().IsUsingMesh() && (moveState.isCoordinated || machineType == MachineType::fff))
		{
			const HeightMap& heightMap = reprap.GetMove().AccessHeightMap();
			const GridDefinition& grid = heightMap.GetGrid();
			const unsigned int minMeshSegments = max<unsigned int>(
					1,
					heightMap.GetMinimumSegments(
						moveState.currentUserPosition[grid.GetAxisNumber(0)] - initialUserPosition[grid.GetAxisNumber(0)],
						moveState.currentUserPosition[grid.GetAxisNumber(1)] - initialUserPosition[grid.GetAxisNumber(1)]
					)
			);
			if (minMeshSegments > moveState.totalSegments)
			{
				moveState.totalSegments = minMeshSegments;
			}
		}
	}

	moveState.doingArcMove = false;
	moveState.linearAxesMentioned = axesMentioned.Intersects(reprap.GetPlatform().GetLinearAxes());
	moveState.rotationalAxesMentioned = axesMentioned.Intersects(reprap.GetPlatform().GetRotationalAxes());
	FinaliseMove(gb);
	UnlockAll(gb);			// allow pause
	err = nullptr;
	return true;
}

// Execute an arc move
// We already have the movement lock and the last move has gone
// Currently, we do not process new babystepping when executing an arc move
// Return true if finished, false if needs to be called again
// If an error occurs, return true with 'err' assigned
bool GCodes::DoArcMove(GCodeBuffer& gb, bool clockwise, const char *& err)
{
	// The plans are XY, ZX and YZ depending on the G17/G18/G19 setting. We must use ZX instead of XZ to get the correct arc direction.
	const unsigned int selectedPlane = gb.LatestMachineState().selectedPlane;
	const unsigned int axis0 = (unsigned int[]){ X_AXIS, Z_AXIS, Y_AXIS }[selectedPlane];
	const unsigned int axis1 = (axis0 + 1) % 3;

	if (moveFractionToSkip > 0.0)
	{
		moveState.initialUserC0 = restartInitialUserC0;
		moveState.initialUserC1 = restartInitialUserC1;
	}
	else
	{
		moveState.initialUserC0 = moveState.currentUserPosition[axis0];
		moveState.initialUserC1 = moveState.currentUserPosition[axis1];
	}

	// Get the axis parameters
	float newAxisPos[2];
	if (gb.Seen(axisLetters[axis0]))
	{
		newAxisPos[0] = gb.GetDistance();
		if (gb.LatestMachineState().axesRelative)
		{
			newAxisPos[0] += moveState.initialUserC0;
		}
		else if (gb.LatestMachineState().g53Active)
		{
			newAxisPos[0] += GetCurrentToolOffset(axis0);
		}
		else if (!gb.LatestMachineState().runningSystemMacro)
		{
			newAxisPos[0] += GetWorkplaceOffset(axis0);
		}
	}
	else
	{
		newAxisPos[0] = moveState.initialUserC0;
	}

	if (gb.Seen(axisLetters[axis1]))
	{
		newAxisPos[1] = gb.GetDistance();
		if (gb.LatestMachineState().axesRelative)
		{
			newAxisPos[1] += moveState.initialUserC1;
		}
		else if (gb.LatestMachineState().g53Active)
		{
			newAxisPos[1] += GetCurrentToolOffset(axis1);
		}
		else if (!gb.LatestMachineState().runningSystemMacro)
		{
			newAxisPos[1] += GetWorkplaceOffset(axis1);
		}
	}
	else
	{
		newAxisPos[1] = moveState.initialUserC1;
	}

	float iParam, jParam;
	if (gb.Seen('R'))
	{
		// We've been given a radius, which takes precedence over I and J parameters
		const float rParam = gb.GetDistance();

		// Get the XY coordinates of the midpoints between the start and end points X and Y distances between start and end points
		const float deltaAxis0 = newAxisPos[0] - moveState.initialUserC0;
		const float deltaAxis1 = newAxisPos[1] - moveState.initialUserC1;

		const float dSquared = fsquare(deltaAxis0) + fsquare(deltaAxis1);	// square of the distance between start and end points

		// The distance between start and end points must not be zero
		if (dSquared == 0.0)
		{
			err = "G2/G3: distance between start and end points must not be zero when specifying a radius";
			return true;
		}

		// The perpendicular must have a real length (possibly zero)
		const float hSquared = fsquare(rParam) - dSquared/4;				// square of the length of the perpendicular from the mid point to the arc centre

		// When the arc is exactly 180deg, rounding error may make hSquared slightly negative instead of zero
		float hDivD;
		if (hSquared >= 0.0)
		{
			hDivD = fastSqrtf(hSquared/dSquared);
		}
		else
		{
			if (hSquared < -0.02 * fsquare(rParam))							// allow the radius to be up to 1% too short
			{
				err = "G2/G3: radius is too small to reach endpoint";
				return true;
			}
			hDivD = 0.0;													// this has the effect of increasing the radius slightly so that the maths works
		}

		// If hDivD is nonzero then there are two possible positions for the arc centre, giving a short arc (less than 180deg) or a long arc (more than 180deg).
		// According to https://www.cnccookbook.com/cnc-g-code-arc-circle-g02-g03/ we should choose the shorter arc if the radius is positive, the longer one if it is negative.
		// If the arc is clockwise then a positive value of h/d gives the smaller arc. If the arc is anticlockwise then it's the other way round.
		if ((clockwise && rParam < 0.0) || (!clockwise && rParam > 0.0))
		{
			hDivD = -hDivD;
		}
		iParam = deltaAxis0/2 + deltaAxis1 * hDivD;
		jParam = deltaAxis1/2 - deltaAxis0 * hDivD;
	}
	else
	{
		if (gb.Seen((char)('I' + axis0)))
		{
			iParam = gb.GetDistance();
		}
		else
		{
			iParam = 0.0;
		}

		if (gb.Seen((char)('I' + axis1)))
		{
			jParam = gb.GetDistance();
		}
		else
		{
			jParam = 0.0;
		}

		if (iParam == 0.0 && jParam == 0.0)			// at least one of IJK must be specified and nonzero
		{
			err = "G2/G3: no I J K or R parameter";
			return true;
		}
	}

	memcpyf(moveState.initialCoords, moveState.coords, numVisibleAxes);

	// Save the arc centre user coordinates for later
	float userArcCentre[2] = { moveState.initialUserC0 + iParam, moveState.initialUserC1 + jParam };

	// Set the new user position
	moveState.currentUserPosition[axis0] = newAxisPos[0];
	moveState.currentUserPosition[axis1] = newAxisPos[1];

	// CNC machines usually do a full circle if the initial and final XY coordinates are the same.
	// Usually this is because X and Y were not given, but repeating the coordinates is permitted.
	const bool wholeCircle = (moveState.initialUserC0 == moveState.currentUserPosition[axis0] && moveState.initialUserC1 == moveState.currentUserPosition[axis1]);

	// Get any additional axes
	AxesBitmap axesMentioned;
	axesMentioned.SetBit(axis0);
	axesMentioned.SetBit(axis1);
	for (size_t axis = 0; axis < numVisibleAxes; axis++)
	{
		if (axis != axis0 && axis != axis1 && gb.Seen(axisLetters[axis]))
		{
			const float moveArg = gb.GetDistance();
			if (gb.LatestMachineState().axesRelative)
			{
				moveState.currentUserPosition[axis] += moveArg * (1.0 - moveFractionToSkip);
			}
			else if (gb.LatestMachineState().g53Active)
			{
				moveState.currentUserPosition[axis] = moveArg + GetCurrentToolOffset(axis);	// g53 ignores tool offsets as well as workplace coordinates
			}
			else if (gb.LatestMachineState().runningSystemMacro)
			{
				moveState.currentUserPosition[axis] = moveArg;									// don't apply workplace offsets to commands in system macros
			}
			else
			{
				moveState.currentUserPosition[axis] = moveArg + GetWorkplaceOffset(axis);
			}
			axesMentioned.SetBit(axis);
		}
	}

	// Check enough axes have been homed
	if (CheckEnoughAxesHomed(axesMentioned))
	{
		err = "G2/G3: insufficient axes homed";
		return true;
	}

	// Compute the initial and final angles. Do this before we possible rotate the coordinates of the arc centre.
	float finalTheta = atan2(moveState.currentUserPosition[axis1] - userArcCentre[1], moveState.currentUserPosition[axis0] - userArcCentre[0]);
	moveState.arcRadius = fastSqrtf(iParam * iParam + jParam * jParam);
	moveState.arcCurrentAngle = atan2(-jParam, -iParam);

	// Transform to machine coordinates and check that it is within limits
#if SUPPORT_COORDINATE_ROTATION
	// Apply coordinate rotation to the final and the centre coordinates
	if (g68Angle != 0.0 && gb.DoingCoordinateRotation())
	{
		float coords[MaxAxes];
		memcpyf(coords, moveState.currentUserPosition, MaxAxes);
		RotateCoordinates(g68Angle, coords);
		ToolOffsetTransform(coords, moveState.coords, axesMentioned);								// set the final position
		RotateCoordinates(g68Angle, userArcCentre);
		finalTheta -= g68Angle * DegreesToRadians;
		moveState.arcCurrentAngle -= g68Angle * DegreesToRadians;
	}
	else
#endif
	{
		ToolOffsetTransform(moveState.currentUserPosition, moveState.coords, axesMentioned);		// set the final position
	}

	if (reprap.GetMove().GetKinematics().LimitPosition(moveState.coords, nullptr, numVisibleAxes, axesVirtuallyHomed, true, limitAxes) != LimitPositionResult::ok)
	{
		err = "G2/G3: outside machine limits";				// abandon the move
		return true;
	}

	// Set up default move parameters
	moveState.checkEndstops = false;
	moveState.reduceAcceleration = false;
	moveState.moveType = 0;
	moveState.tool = reprap.GetCurrentTool();
	moveState.isCoordinated = true;

	// Set up the arc centre coordinates and record which axes behave like an X axis.
	// The I and J parameters are always relative to present position.
	// For X and Y we need to set up the arc centre for each axis that X or Y is mapped to.
	const AxesBitmap axis0Mapping = reprap.GetCurrentAxisMapping(axis0);
	const AxesBitmap axis1Mapping = reprap.GetCurrentAxisMapping(axis1);
	for (size_t axis = 0; axis < numVisibleAxes; ++axis)
	{
		if (axis0Mapping.IsBitSet(axis))
		{
			moveState.arcCentre[axis] = (userArcCentre[0] * axisScaleFactors[axis]) + currentBabyStepOffsets[axis] - Tool::GetOffset(reprap.GetCurrentTool(), axis);
		}
		else if (axis1Mapping.IsBitSet(axis))
		{
			moveState.arcCentre[axis] = (userArcCentre[1] * axisScaleFactors[axis]) + currentBabyStepOffsets[axis] - Tool::GetOffset(reprap.GetCurrentTool(), axis);
		}
	}

	err = LoadExtrusionAndFeedrateFromGCode(gb, true);
	if (err != nullptr)
	{
		return true;
	}

	if (buildObjects.IsFirstMoveSincePrintingResumed())
	{
		if (moveState.hasPositiveExtrusion)					// check whether this is the first move after skipping an object and is extruding
		{
			if (TravelToStartPoint(gb))							// don't start a printing move from the wrong point
			{
				buildObjects.DoneMoveSincePrintingResumed();
			}
			return false;
		}
		else
		{
			buildObjects.DoneMoveSincePrintingResumed();
		}
	}

#if TRACK_OBJECT_NAMES
	if (moveState.hasPositiveExtrusion)
	{
		//TODO ideally we should calculate the min and max X and Y coordinates of the entire arc here and call UpdateObjectCoordinates twice.
		// But it is currently very rare to use G2/G3 with extrusion, so for now we don't bother.
		buildObjects.UpdateObjectCoordinates(moveState.currentUserPosition, AxesBitmap::MakeLowestNBits(2));
	}
#endif

#if SUPPORT_LASER
	if (machineType == MachineType::laser)
	{
		if (gb.Seen('S'))
		{
			moveState.laserPwmOrIoBits.laserPwm = ConvertLaserPwm(gb.GetFValue());
		}
		else if (laserPowerSticky)
		{
			// leave the laser PWM alone because this is what LaserWeb expects
		}
		else
		{
			moveState.laserPwmOrIoBits.laserPwm = 0;
		}
	}
# if SUPPORT_IOBITS
	else
# endif
#endif
#if SUPPORT_IOBITS
	{
		// Update the iobits parameter
		if (gb.Seen('P'))
		{
			moveState.laserPwmOrIoBits.ioBits = (IoBits_t)gb.GetIValue();
		}
		else
		{
			// Leave moveBuffer.ioBits alone so that we keep the previous value
		}
	}
#endif

	moveState.usePressureAdvance = moveState.hasPositiveExtrusion;

	// Calculate the total angle moved, which depends on which way round we are going
	float totalArc;
	if (wholeCircle)
	{
		totalArc = TwoPi;
	}
	else
	{
		totalArc = (clockwise) ? moveState.arcCurrentAngle - finalTheta : finalTheta - moveState.arcCurrentAngle;
		if (totalArc < 0.0)
		{
			totalArc += TwoPi;
		}
	}

	// Compute how many segments to use
	// For the arc to deviate up to MaxArcDeviation from the ideal, the segment length should be sqrtf(8 * arcRadius * MaxArcDeviation + fsquare(MaxArcDeviation))
	// We leave out the square term because it is very small
	// In CNC applications even very small deviations can be visible, so we use a smaller segment length at low speeds
	const float arcSegmentLength = constrain<float>
									(	min<float>(fastSqrtf(8 * moveState.arcRadius * MaxArcDeviation), moveState.feedRate * StepClockRate * (1.0/MinArcSegmentsPerSec)),
										MinArcSegmentLength,
										MaxArcSegmentLength
									);
	moveState.totalSegments = max<unsigned int>((unsigned int)((moveState.arcRadius * totalArc)/arcSegmentLength + 0.8), 1u);
	moveState.arcAngleIncrement = totalArc/moveState.totalSegments;
	if (clockwise)
	{
		moveState.arcAngleIncrement = -moveState.arcAngleIncrement;
	}
	moveState.angleIncrementSine = sinf(moveState.arcAngleIncrement);
	moveState.angleIncrementCosine = cosf(moveState.arcAngleIncrement);
	moveState.segmentsTillNextFullCalc = 0;

	moveState.arcAxis0 = axis0;
	moveState.arcAxis1 = axis1;
	moveState.doingArcMove = true;
	moveState.xyPlane = (selectedPlane == 0);
	moveState.linearAxesMentioned = axesMentioned.Intersects(reprap.GetPlatform().GetLinearAxes());
	moveState.rotationalAxesMentioned = axesMentioned.Intersects(reprap.GetPlatform().GetRotationalAxes());
	FinaliseMove(gb);
	UnlockAll(gb);			// allow pause
//	debugPrintf("Radius %.2f, initial angle %.1f, increment %.1f, segments %u\n",
//				arcRadius, arcCurrentAngle * RadiansToDegrees, arcAngleIncrement * RadiansToDegrees, segmentsLeft);
	return true;
}

// Adjust the move parameters to account for segmentation and/or part of the move having been done already
void GCodes::FinaliseMove(GCodeBuffer& gb) noexcept
{
	moveState.canPauseAfter = !moveState.checkEndstops && !moveState.doingArcMove;		// pausing during an arc move isn't safe because the arc centre get recomputed incorrectly when we resume
	moveState.filePos = (&gb == fileGCode) ? gb.GetFilePosition() : noFilePosition;
	gb.MotionCommanded();

	if (buildObjects.IsCurrentObjectCancelled())
	{
#if SUPPORT_LASER
		if (machineType == MachineType::laser)
		{
			platform.SetLaserPwm(0);
		}
#endif
	}
	else
	{
		if (moveState.totalSegments > 1)
		{
			moveState.segMoveState = SegmentedMoveState::active;
			gb.SetState(GCodeState::waitingForSegmentedMoveToGo);

			for (size_t extruder = 0; extruder < numExtruders; ++extruder)
			{
				moveState.coords[ExtruderToLogicalDrive(extruder)] /= moveState.totalSegments;	// change the extrusion to extrusion per segment
			}

			if (moveFractionToSkip != 0.0)
			{
				const float fseg = floor(moveState.totalSegments * moveFractionToSkip);		// round down to the start of a move
				segmentsLeftToStartAt = moveState.totalSegments - (unsigned int)fseg;
				firstSegmentFractionToSkip = (moveFractionToSkip * moveState.totalSegments) - fseg;
				NewMoveAvailable();
				return;
			}
		}
		else
		{
			moveState.segMoveState = SegmentedMoveState::inactive;
		}

		segmentsLeftToStartAt = moveState.totalSegments;
		firstSegmentFractionToSkip = moveFractionToSkip;

		NewMoveAvailable();
	}
}

// Set up a move to travel to the resume point. Return true if successful, false if needs to be called again.
// By the time this is called, the user position has been overwritten with the final position of the pending move, so we can't use it.
// But the expected position was saved by buildObjects when the state changed from printing a cancelled object to printing a live object.
bool GCodes::TravelToStartPoint(GCodeBuffer& gb) noexcept
{
	if (!LockMovementAndWaitForStandstill(gb))				// update the user position from the machine position
	{
		return false;
	}

	SetMoveBufferDefaults();
	ToolOffsetTransform(moveState.currentUserPosition, moveState.initialCoords);
	ToolOffsetTransform(buildObjects.GetInitialPosition().moveCoords, moveState.coords);
	moveState.feedRate = buildObjects.GetInitialPosition().feedRate;
	moveState.tool = reprap.GetCurrentTool();
	moveState.linearAxesMentioned = moveState.rotationalAxesMentioned = true;		// assume that both linear and rotational axes might be moving
	NewSingleSegmentMoveAvailable();
	return true;
}

// The Move class calls this function to find what to do next. It takes its own copy of the move because it adjusts the coordinates.
// Returns true if a new move was copied to 'm'.
bool GCodes::ReadMove(RawMove& m) noexcept
{
	if (moveState.segmentsLeft == 0)
	{
		return false;
	}

	while (true)		// loop while we skip move segments
	{
		m = moveState;

		if (moveState.segmentsLeft == 1)
		{
			// If there is just 1 segment left, it doesn't matter if it is an arc move or not, just move to the end position
			if (segmentsLeftToStartAt == 1 && firstSegmentFractionToSkip != 0.0)	// if this is the segment we are starting at and we need to skip some of it
			{
				// Reduce the extrusion by the amount to be skipped
				for (size_t extruder = 0; extruder < numExtruders; ++extruder)
				{
					m.coords[ExtruderToLogicalDrive(extruder)] *= (1.0 - firstSegmentFractionToSkip);
				}
			}
			m.proportionDone = 1.0;
			if (moveState.doingArcMove)
			{
				m.canPauseAfter = true;					// we can pause after the final segment of an arc move
			}
			ClearMove();
		}
		else
		{
			// This move needs to be divided into 2 or more segments
			// Do the axes
			AxesBitmap axisMap0, axisMap1;
			if (moveState.doingArcMove)
			{
				moveState.arcCurrentAngle += moveState.arcAngleIncrement;
				if (moveState.segmentsTillNextFullCalc == 0)
				{
					// Do the full calculation
					moveState.segmentsTillNextFullCalc = SegmentsPerFulArcCalculation;
					moveState.currentAngleCosine = cosf(moveState.arcCurrentAngle);
					moveState.currentAngleSine = sinf(moveState.arcCurrentAngle);
				}
				else
				{
					// Speed up the computation by doing two multiplications and an addition or subtraction instead of a sine or cosine
					--moveState.segmentsTillNextFullCalc;
					const float newCosine = moveState.currentAngleCosine * moveState.angleIncrementCosine - moveState.currentAngleSine   * moveState.angleIncrementSine;
					const float newSine   = moveState.currentAngleSine   * moveState.angleIncrementCosine + moveState.currentAngleCosine * moveState.angleIncrementSine;
					moveState.currentAngleCosine = newCosine;
					moveState.currentAngleSine = newSine;
				}
				axisMap0 = Tool::GetAxisMapping(moveState.tool, moveState.arcAxis0);
				axisMap1 = Tool::GetAxisMapping(moveState.tool, moveState.arcAxis1);
				moveState.cosXyAngle = (moveState.xyPlane) ? moveState.angleIncrementCosine : 1.0;
			}

			for (size_t drive = 0; drive < numVisibleAxes; ++drive)
			{
				if (moveState.doingArcMove && axisMap1.IsBitSet(drive))
				{
					// Axis1 or a substitute in the selected plane
					moveState.initialCoords[drive] = moveState.arcCentre[drive] + moveState.arcRadius * axisScaleFactors[drive] * moveState.currentAngleSine;
				}
				else if (moveState.doingArcMove && axisMap0.IsBitSet(drive))
				{
					// Axis0 or a substitute in the selected plane
					moveState.initialCoords[drive] = moveState.arcCentre[drive] + moveState.arcRadius * axisScaleFactors[drive] * moveState.currentAngleCosine;
				}
				else
				{
					// This axis is not moving in an arc
					const float movementToDo = (moveState.coords[drive] - moveState.initialCoords[drive])/moveState.segmentsLeft;
					moveState.initialCoords[drive] += movementToDo;
				}
				m.coords[drive] = moveState.initialCoords[drive];
			}

			if (segmentsLeftToStartAt < moveState.segmentsLeft)
			{
				// We are resuming a print part way through a move and we printed this segment already
				--moveState.segmentsLeft;
				continue;
			}

			// Limit the end position at each segment. This is needed for arc moves on any printer, and for [segmented] straight moves on SCARA printers.
			if (reprap.GetMove().GetKinematics().LimitPosition(m.coords, nullptr, numVisibleAxes, axesVirtuallyHomed, true, limitAxes) != LimitPositionResult::ok)
			{
				moveState.segMoveState = SegmentedMoveState::aborted;
				moveState.doingArcMove = false;
				moveState.segmentsLeft = 0;
				return false;
			}

			if (segmentsLeftToStartAt == moveState.segmentsLeft && firstSegmentFractionToSkip != 0.0)	// if this is the segment we are starting at and we need to skip some of it
			{
				// Reduce the extrusion by the amount to be skipped
				for (size_t extruder = 0; extruder < numExtruders; ++extruder)
				{
					m.coords[ExtruderToLogicalDrive(extruder)] *= (1.0 - firstSegmentFractionToSkip);
				}
			}
			--moveState.segmentsLeft;

			m.proportionDone = moveState.GetProportionDone();
		}

		return true;
	}
}

void GCodes::ClearMove() noexcept
{
	TaskCriticalSectionLocker lock;				// make sure that other tasks sees a consistent memory state

	moveState.segmentsLeft = 0;
	moveState.segMoveState = SegmentedMoveState::inactive;
	moveState.doingArcMove = false;
	moveState.checkEndstops = false;
	moveState.reduceAcceleration = false;
	moveState.moveType = 0;
	moveState.applyM220M221 = false;
	moveFractionToSkip = 0.0;
}

// Flag that a new single-segment move is available for consumption by the Move subsystem
// Code that sets up a new move should ensure that segmentsLeft is zero, then set up all the move parameters,
// then call this function to update SegmentsLeft safely in a multi-threaded environment
void GCodes::NewSingleSegmentMoveAvailable() noexcept
{
	moveState.totalSegments = 1;
	__DMB();									// make sure that all the move details have been written first
	moveState.segmentsLeft = 1;					// set the number of segments to indicate that a move is available to be taken
	reprap.GetMove().MoveAvailable();			// notify the Move task that we have a move
}

// Flag that a new move is available for consumption by the Move subsystem
// This version is for when totalSegments has already be set up.
void GCodes::NewMoveAvailable() noexcept
{
	const unsigned int sl = moveState.totalSegments;
	__DMB();									// make sure that the move details have been written first
	moveState.segmentsLeft = sl;				// set the number of segments to indicate that a move is available to be taken
	reprap.GetMove().MoveAvailable();			// notify the Move task that we have a move
}

// Cancel any macro or print in progress
void GCodes::AbortPrint(GCodeBuffer& gb) noexcept
{
	(void)gb.AbortFile(true);					// stop executing any files or macros that this GCodeBuffer is running
	if (&gb == fileGCode)						// if the current command came from a file being printed
	{
		StopPrint(StopPrintReason::abort);
	}
}

// Cancel everything
void GCodes::EmergencyStop() noexcept
{
	for (GCodeBuffer *gbp : gcodeSources)
	{
		if (gbp != nullptr)
		{
			AbortPrint(*gbp);
		}
	}
#if SUPPORT_LASER
	moveState.laserPwmOrIoBits.laserPwm = 0;
#endif
}

// Simplified version of DoFileMacro, see below
bool GCodes::DoFileMacro(GCodeBuffer& gb, const char* fileName, bool reportMissing, int codeRunning) noexcept
{
	VariableSet vars;
	if (codeRunning >= 0)
	{
		gb.AddParameters(vars, codeRunning);
	}
	return DoFileMacro(gb, fileName, reportMissing, codeRunning, vars);
}

// Run a file macro. Prior to calling this, 'state' must be set to the state we want to enter when the macro has been completed.
// Return true if the file was found or it wasn't and we were asked to report that fact.
// 'codeRunning' is the G or M command we are running, or 0 for a tool change file. In particular:
// 501 = running M501
// 502 = running M502
// 98 = running a macro explicitly via M98
// otherwise it is either the G- or M-code being executed, or ToolChangeMacroCode for a tool change file, or SystemMacroCode for another system file
bool GCodes::DoFileMacro(GCodeBuffer& gb, const char* fileName, bool reportMissing, int codeRunning, VariableSet& initialVariables) noexcept
{
	if (codeRunning != AsyncSystemMacroCode && &gb == fileGCode && gb.LatestMachineState().GetPrevious() == nullptr)
	{
		// This macro was invoked directly from the print file by M98, G28, G29, G32 etc. so record the file location of that command so that we can restart it
		printFilePositionAtMacroStart = gb.GetFilePosition();
	}

#if HAS_SBC_INTERFACE
	if (reprap.UsingSbcInterface())
	{
		if (!gb.RequestMacroFile(fileName, gb.IsBinary() && codeRunning != AsyncSystemMacroCode))
		{
			if (reportMissing)
			{
				MessageType mt = (gb.IsBinary() && codeRunning != SystemHelperMacroCode)
									? (MessageType)(gb.GetResponseMessageType() | WarningMessageFlag | PushFlag)
										: WarningMessage;
				platform.MessageF(mt, "Macro file %s not found\n", fileName);
				return true;
			}

			return false;
		}

		if (!Push(gb, false))
		{
			gb.AbortFile(false, true);
			return true;
		}
		gb.GetVariables().AssignFrom(initialVariables);
		gb.StartNewFile();
		if (gb.IsMacroEmpty())
		{
			gb.SetFileFinished();
		}
	}
	else
#endif
	{
#if HAS_MASS_STORAGE || HAS_EMBEDDED_FILES
		FileStore * const f = platform.OpenSysFile(fileName, OpenMode::read);
		if (f == nullptr)
		{
			if (reportMissing)
			{
				platform.MessageF(WarningMessage, "Macro file %s not found\n", fileName);
				return true;
			}
			return false;
		}

		if (!Push(gb, false))
		{
			f->Close();
			return true;
		}
		gb.GetVariables().AssignFrom(initialVariables);
		gb.LatestMachineState().fileState.Set(f);
		gb.StartNewFile();
		gb.GetFileInput()->Reset(gb.LatestMachineState().fileState);
#else
		if (reportMissing)
		{
			platform.MessageF(WarningMessage, "Macro file %s not found\n", fileName);
		}
		return reportMissing;
#endif
	}

#if HAS_SBC_INTERFACE || HAS_MASS_STORAGE || HAS_EMBEDDED_FILES
	gb.LatestMachineState().doingFileMacro = true;

	// The following three flags need to be inherited in the case that a system macro calls another macro, e.g.homeall.g calls homez.g. The Push call copied them over already.
	switch (codeRunning)
	{
	case 501:
		gb.LatestMachineState().runningM501 = true;
		gb.LatestMachineState().runningSystemMacro = true;			// running a system macro e.g. homing, so don't use workplace coordinates
		break;

	case 502:
		gb.LatestMachineState().runningM502 = true;
		gb.LatestMachineState().runningSystemMacro = true;			// running a system macro e.g. homing, so don't use workplace coordinates
		break;

	case SystemHelperMacroCode:
	case AsyncSystemMacroCode:
	case ToolChangeMacroCode:
	case 29:
	case 32:
		gb.LatestMachineState().runningSystemMacro = true;			// running a system macro e.g. homing, so don't use workplace coordinates
		break;

	default:
		break;
	}

	gb.SetState(GCodeState::normal);
	gb.Init();

# if HAS_SBC_INTERFACE
	if (!reprap.UsingSbcInterface() && codeRunning != AsyncSystemMacroCode)
# endif
	{
		// Don't notify DSF when files are requested asynchronously, it creates excessive traffic
		reprap.InputsUpdated();
	}
	return true;
#endif
}

// Return true if the macro being executed by fileGCode was restarted
bool GCodes::GetMacroRestarted() const noexcept
{
	const GCodeMachineState& ms = fileGCode->LatestMachineState();
	return ms.doingFileMacro && ms.GetPrevious() != nullptr && ms.GetPrevious()->firstCommandAfterRestart;
}

void GCodes::FileMacroCyclesReturn(GCodeBuffer& gb) noexcept
{
	if (gb.IsDoingFileMacro())
	{
#if HAS_SBC_INTERFACE
		if (reprap.UsingSbcInterface())
		{
			gb.AbortFile(false);
		}
		else
#endif
		{
#if HAS_MASS_STORAGE || HAS_EMBEDDED_FILES
			FileData &file = gb.LatestMachineState().fileState;
			gb.GetFileInput()->Reset(file);
			file.Close();

			gb.PopState(false);
#endif
		}

		gb.Init();
	}
}

// Home one or more of the axes
// 'reply' is only written if there is an error.
GCodeResult GCodes::DoHome(GCodeBuffer& gb, const StringRef& reply) THROWS(GCodeException)
{
	if (!LockMovementAndWaitForStandstill(gb))
	{
		return GCodeResult::notFinished;
	}

#if SUPPORT_ROLAND
	// Deal with a Roland configuration
	if (reprap.GetRoland()->Active())
	{
		bool rolHome = reprap.GetRoland()->ProcessHome();
		if (rolHome)
		{
			for(size_t axis = 0; axis < AXES; axis++)
			{
				axisIsHomed[axis] = true;
			}
		}
		return rolHome;
	}
#endif

	// We have the movement lock so we have exclusive access to the homing flags
	if (toBeHomed.IsNonEmpty())
	{
		reply.copy("G28 may not be used within a homing file");
		return GCodeResult::error;
	}

	// Find out which axes we have been asked to home
	for (size_t axis = 0; axis < numTotalAxes; ++axis)
	{
		if (gb.Seen(axisLetters[axis]))
		{
			toBeHomed.SetBit(axis);
			SetAxisNotHomed(axis);
		}
	}

	if (toBeHomed.IsEmpty())
	{
		SetAllAxesNotHomed();		// homing everything
		toBeHomed = AxesBitmap::MakeLowestNBits(numVisibleAxes);
	}

	gb.SetState(GCodeState::homing1);
	return GCodeResult::ok;
}

// This is called to execute a G30.
// It sets wherever we are as the probe point P (probePointIndex) then probes the bed, or gets all its parameters from the arguments.
// If X or Y are specified, use those; otherwise use the machine's coordinates.  If no Z is specified use the machine's coordinates.
// If it is specified and is greater than SILLY_Z_VALUE (i.e. greater than -9999.0) then that value is used.
// If it's less than SILLY_Z_VALUE the bed is probed and that value is used.
// We already own the movement lock before this is called.
GCodeResult GCodes::ExecuteG30(GCodeBuffer& gb, const StringRef& reply)
{
	g30SValue = (gb.Seen('S')) ? gb.GetIValue() : -4;		// S-4 or lower is equivalent to having no S parameter
	if (g30SValue == -2 && reprap.GetCurrentTool() == nullptr)
	{
		reply.copy("G30 S-2 commanded with no tool selected");
		return GCodeResult::error;
	}

	g30HValue = (gb.Seen('H')) ? gb.GetFValue() : 0.0;
	g30ProbePointIndex = -1;
	bool seenP = false;
	gb.TryGetIValue('P', g30ProbePointIndex, seenP);
	if (seenP)
	{
		if (g30ProbePointIndex < 0 || g30ProbePointIndex >= (int)MaxProbePoints)
		{
			reply.copy("Z probe point index out of range");
			return GCodeResult::error;
		}
		else
		{
			// Set the specified probe point index to the specified coordinates
			const float x = (gb.Seen(axisLetters[X_AXIS])) ? gb.GetFValue() : moveState.currentUserPosition[X_AXIS];
			const float y = (gb.Seen(axisLetters[Y_AXIS])) ? gb.GetFValue() : moveState.currentUserPosition[Y_AXIS];
			const float z = (gb.Seen(axisLetters[Z_AXIS])) ? gb.GetFValue() : moveState.currentUserPosition[Z_AXIS];
			reprap.GetMove().SetXYBedProbePoint((size_t)g30ProbePointIndex, x, y);

			if (z > SILLY_Z_VALUE)
			{
				// Just set the height error to the specified Z coordinate
				reprap.GetMove().SetZBedProbePoint((size_t)g30ProbePointIndex, z, false, false);
				if (g30SValue >= -1)
				{
					return GetGCodeResultFromError(reprap.GetMove().FinishedBedProbing(g30SValue, reply));
				}
			}
			else
			{
				// Do a Z probe at the specified point.
				const auto zp = SetZProbeNumber(gb, 'K');		// may throw, so do this before changing the state
				gb.SetState(GCodeState::probingAtPoint0);
				if (zp->GetProbeType() != ZProbeType::blTouch)
				{
					DeployZProbe(gb);
				}
			}
		}
	}
	else
	{
		// G30 without P parameter. This probes the current location starting from the current position.
		// If S=-1 it just reports the stopped height, else it resets the Z origin.
		const auto zp = SetZProbeNumber(gb, 'K');				// may throw, so do this before changing the state
		InitialiseTaps(zp->HasTwoProbingSpeeds());
		gb.SetState(GCodeState::probingAtPoint2a);
		if (zp->GetProbeType() != ZProbeType::blTouch)
		{
			DeployZProbe(gb);
		}
	}
	return GCodeResult::ok;
}

// Set up currentZProbeNumber and return the probe
ReadLockedPointer<ZProbe> GCodes::SetZProbeNumber(GCodeBuffer& gb, char probeLetter) THROWS(GCodeException)
{
	const uint32_t probeNumber = (gb.Seen(probeLetter)) ? gb.GetLimitedUIValue(probeLetter, MaxZProbes) : 0;
	auto zp = reprap.GetPlatform().GetEndstops().GetZProbe(probeNumber);
	if (zp.IsNull())
	{
		throw GCodeException(gb.GetLineNumber(), -1, "Z probe %" PRIu32 " not found", probeNumber);
	}
	currentZProbeNumber = (uint8_t)probeNumber;
	return zp;
}

// Decide which device to display a message box on
MessageType GCodes::GetMessageBoxDevice(GCodeBuffer& gb) const
{
	MessageType mt = gb.GetResponseMessageType();
	if (mt == GenericMessage)
	{
		// Command source was the file being printed, or a trigger. Send the message to PanelDue if there is one, else to the web server.
		mt = (lastAuxStatusReportType >= 0) ? AuxMessage : HttpMessage;
	}
	return mt;
}

void GCodes::DoManualProbe(GCodeBuffer& gb, const char *message, const char *title, const AxesBitmap axes)
{
	if (Push(gb, true))													// stack the machine state including the file position and set the state to GCodeState::normal
	{
		gb.WaitForAcknowledgement();									// flag that we are waiting for acknowledgement
		const MessageType mt = GetMessageBoxDevice(gb);
		platform.SendAlert(mt, message, title, 2, 0.0, axes);
	}
}

// Do a manual bed probe. On entry the state variable is the state we want to return to when the user has finished adjusting the height.
void GCodes::DoManualBedProbe(GCodeBuffer& gb)
{
	DoManualProbe(gb, "Adjust height until the nozzle just touches the bed, then press OK", "Manual bed probing", AxesBitmap::MakeFromBits(Z_AXIS));
}

// Start probing the grid, returning true if we didn't because of an error.
// Prior to calling this the movement system must be locked.
GCodeResult GCodes::ProbeGrid(GCodeBuffer& gb, const StringRef& reply)
{
	if (!defaultGrid.IsValid())
	{
		reply.copy("No valid grid defined for bed probing");
		return GCodeResult::error;
	}

	if (!AllAxesAreHomed())
	{
		reply.copy("Must home printer before bed probing");
		return GCodeResult::error;
	}

	const auto zp = SetZProbeNumber(gb, 'K');			// may throw, so do this before changing the state

	reprap.GetMove().AccessHeightMap().SetGrid(defaultGrid);
	ClearBedMapping();
	gridAxis0index = gridAxis1index = 0;

	gb.SetState(GCodeState::gridProbing1);
	if (zp->GetProbeType() != ZProbeType::blTouch)
	{
		DeployZProbe(gb);
	}
	return GCodeResult::ok;
}

#if HAS_MASS_STORAGE || HAS_SBC_INTERFACE

GCodeResult GCodes::LoadHeightMap(GCodeBuffer& gb, const StringRef& reply)
{
	ClearBedMapping();

	String<MaxFilenameLength> heightMapFileName;
	bool seen = false;
	gb.TryGetQuotedString('P', heightMapFileName.GetRef(), seen);
	if (!seen)
	{
		heightMapFileName.copy(DefaultHeightMapFile);
	}

	String<MaxFilenameLength> fullName;
	platform.MakeSysFileName(fullName.GetRef(), heightMapFileName.c_str());
	FileStore * const f = MassStorage::OpenFile(fullName.c_str(), OpenMode::read, 0);
	if (f == nullptr)
	{
		reply.printf("Height map file %s not found", fullName.c_str());
		return GCodeResult::error;
	}
	reply.printf("Failed to load height map from file %s: ", fullName.c_str());	// set up error message to append to

	const bool err = reprap.GetMove().LoadHeightMapFromFile(f, fullName.c_str(), reply);
	f->Close();

	ActivateHeightmap(!err);
	if (err)
	{
		return GCodeResult::error;
	}

	reply.Clear();						// get rid of the error message
	if (!zDatumSetByProbing && platform.GetZProbeOrDefault(0)->GetProbeType() != ZProbeType::none)	//TODO store Z probe number in height map
	{
		reply.copy("the height map was loaded when the current Z=0 datum was not determined by probing. This may result in a height offset.");
		return GCodeResult::warning;
	}

	return GCodeResult::ok;
}

// Save the height map and append the success or error message to 'reply', returning true if an error occurred
bool GCodes::TrySaveHeightMap(const char *filename, const StringRef& reply) const noexcept
{
	String<MaxFilenameLength> fullName;
	platform.MakeSysFileName(fullName.GetRef(), filename);
	FileStore * const f = MassStorage::OpenFile(fullName.c_str(), OpenMode::write, 0);
	bool err;
	if (f == nullptr)
	{
		reply.catf("Failed to create height map file %s", fullName.c_str());
		err = true;
	}
	else
	{
		err = reprap.GetMove().SaveHeightMapToFile(f, fullName.c_str());
		f->Close();
		if (err)
		{
			MassStorage::Delete(fullName.c_str(), false);
			reply.catf("Failed to save height map to file %s", fullName.c_str());
		}
		else
		{
			reply.catf("Height map saved to file %s", fullName.c_str());
		}
	}
	return err;
}

// Save the height map to the file specified by P parameter
GCodeResult GCodes::SaveHeightMap(GCodeBuffer& gb, const StringRef& reply) const
{
	// No need to check if we're using the SBC interface here, because TrySaveHeightMap does that already
	if (gb.Seen('P'))
	{
		String<MaxFilenameLength> heightMapFileName;
		gb.GetQuotedString(heightMapFileName.GetRef());
		return GetGCodeResultFromError(TrySaveHeightMap(heightMapFileName.c_str(), reply));
	}
	return GetGCodeResultFromError(TrySaveHeightMap(DefaultHeightMapFile, reply));
}

#endif

// Stop using bed compensation
void GCodes::ClearBedMapping()
{
	reprap.GetMove().SetIdentityTransform();
	reprap.GetMove().GetCurrentUserPosition(moveState.coords, 0, reprap.GetCurrentTool());
	ToolOffsetInverseTransform(moveState.coords, moveState.currentUserPosition);		// update user coordinates to remove any height map offset there was at the current position
}

// Return the current coordinates as a printable string.
// Coordinates are updated at the end of each movement, so this won't tell you where you are mid-movement.
void GCodes::GetCurrentCoordinates(const StringRef& s) const noexcept
{
	// Start with the axis coordinates
	s.Clear();
	for (size_t axis = 0; axis < numVisibleAxes; ++axis)
	{
		// Don't put a space after the colon in the response, it confuses Pronterface
		s.catf("%c:%.3f ", axisLetters[axis], (double)HideNan(GetUserCoordinate(axis)));
	}

	// Now the virtual extruder position, for Octoprint
	s.catf("E:%.3f ", (double)virtualExtruderPosition);

	// Now the extruder coordinates
	for (size_t i = 0; i < numExtruders; i++)
	{
		s.catf("E%u:%.1f ", i, (double)reprap.GetMove().LiveCoordinate(ExtruderToLogicalDrive(i), reprap.GetCurrentTool()));
	}

	// Print the axis stepper motor positions as Marlin does, as an aid to debugging.
	// Don't bother with the extruder endpoints, they are zero after any non-extruding move.
	s.cat("Count");
	for (size_t i = 0; i < numVisibleAxes; ++i)
	{
		s.catf(" %" PRIi32, reprap.GetMove().GetEndPoint(i));
	}

	// Add the machine coordinates because they may be different from the user coordinates under some conditions
	s.cat(" Machine");
	float machineCoordinates[MaxAxes];
	ToolOffsetTransform(moveState.currentUserPosition, machineCoordinates);
	for (size_t axis = 0; axis < numVisibleAxes; ++axis)
	{
		s.catf(" %.3f", (double)HideNan(machineCoordinates[axis]));
	}

	// Add the bed compensation
	const float machineZ = machineCoordinates[Z_AXIS];
	reprap.GetMove().AxisAndBedTransform(machineCoordinates, reprap.GetCurrentTool(), true);
	s.catf(" Bed comp %.3f", (double)(machineCoordinates[Z_AXIS] - machineZ));
}

#if HAS_MASS_STORAGE || HAS_EMBEDDED_FILES
// Set up a file to print, but don't print it yet.
// If successful return true, else write an error message to reply and return false
bool GCodes::QueueFileToPrint(const char* fileName, const StringRef& reply) noexcept
{
	FileStore * const f = platform.OpenFile(Platform::GetGCodeDir(), fileName, OpenMode::read);
	if (f != nullptr)
	{
		fileToPrint.Set(f);
		return true;
	}

	reply.printf("GCode file \"%s\" not found\n", fileName);
	return false;
}
#endif

// Start printing the file already selected. We must hold the movement lock and wait for all moves to finish before calling this, because of the call to ResetMoveCounters.
void GCodes::StartPrinting(bool fromStart) noexcept
{
#if HAS_MASS_STORAGE || HAS_SBC_INTERFACE || HAS_EMBEDDED_FILES
	fileOffsetToPrint = 0;
#endif
	restartMoveFractionDone = 0.0;

	buildObjects.Init();
	reprap.GetMove().ResetMoveCounters();

	if (fromStart)															// if not resurrecting a print
	{
		fileGCode->LatestMachineState().volumetricExtrusion = false;		// default to non-volumetric extrusion
		virtualExtruderPosition = 0.0;
	}

	for (size_t extruder = 0; extruder < MaxExtruders; extruder++)
	{
		rawExtruderTotalByDrive[extruder] = 0.0;
	}
	rawExtruderTotal = 0.0;
	reprap.GetMove().ResetExtruderPositions();

#if HAS_SBC_INTERFACE
	if (!reprap.UsingSbcInterface())
#endif
	{
#if HAS_MASS_STORAGE || HAS_EMBEDDED_FILES
		fileGCode->OriginalMachineState().fileState.MoveFrom(fileToPrint);
		fileGCode->GetFileInput()->Reset(fileGCode->OriginalMachineState().fileState);
#endif
	}
	fileGCode->StartNewFile();

	reprap.GetPrintMonitor().StartedPrint();
	platform.MessageF(LogWarn,
						(IsSimulating()) ? "Started simulating printing file %s\n" : "Started printing file %s\n",
							reprap.GetPrintMonitor().GetPrintingFilename());
	if (fromStart)
	{
		fileGCode->LatestMachineState().selectedPlane = 0;					// default G2 and G3 moves to XY plane
		DoFileMacro(*fileGCode, START_G, false, AsyncSystemMacroCode);		// get fileGCode to execute the start macro so that any M82/M83 codes will be executed in the correct context
	}
	else
	{
		fileGCode->LatestMachineState().firstCommandAfterRestart = true;
	}
}

// Function to handle dwell delays. Returns true for dwell finished, false otherwise.
GCodeResult GCodes::DoDwell(GCodeBuffer& gb) THROWS(GCodeException)
{
	// Wait for all the queued moves to stop. Only do this if motion has been commanded from this GCode stream since we last waited for motion to stop.
	// This is so that G4 can be used in a trigger or daemon macro file without pausing motion, when the macro doesn't itself command any motion.
	if (gb.WasMotionCommanded())
	{
		if (!LockMovementAndWaitForStandstill(gb))
		{
			return GCodeResult::notFinished;
		}
	}

	UnlockAll(gb);																	// don't hang on to the movement lock while we delay

	const int32_t dwell = (gb.Seen('S')) ? (int32_t)(gb.GetFValue() * 1000.0)		// S values are in seconds
							: (gb.Seen('P')) ? gb.GetIValue()						// P value are in milliseconds
								: 0;
	if (dwell <= 0)
	{
		return GCodeResult::ok;
	}

#if SUPPORT_ROLAND
	// Deal with a Roland configuration
	if (reprap.GetRoland()->Active())
	{
		return reprap.GetRoland()->ProcessDwell(dwell);
	}
#endif

	if (   IsSimulating()															// if we are simulating then simulate the G4...
		&& &gb != daemonGCode														// ...unless it comes from the daemon...
		&& &gb != triggerGCode														// ...or a trigger...
		&& (&gb == fileGCode || !exitSimulationWhenFileComplete)					// ...or we are simulating a file and this command doesn't come from the file
	   )
	{
		simulationTime += (float)dwell * 0.001;
		return GCodeResult::ok;
	}

	return (gb.DoDwellTime((uint32_t)dwell)) ? GCodeResult::ok : GCodeResult::notFinished;
}

// Get the tool specified by the P parameter, or the current tool if no P parameter
ReadLockedPointer<Tool> GCodes::GetSpecifiedOrCurrentTool(GCodeBuffer& gb) THROWS(GCodeException)
{
	int tNumber;
	if (gb.Seen('P'))
	{
		tNumber = (int)gb.GetUIValue();
	}
	else
	{
		tNumber = reprap.GetCurrentToolNumber();
		if (tNumber < 0)
		{
			throw GCodeException(gb.GetLineNumber(), -1, "No tool number given and no current tool");
		}
	}

	ReadLockedPointer<Tool> tool = reprap.GetTool(tNumber);
	if (tool.IsNull())
	{
		throw GCodeException(gb.GetLineNumber(), -1, "Invalid tool number");
	}
	return tool;
}

// Set offset, working and standby temperatures for a tool. i.e. handle a G10 or M568.
GCodeResult GCodes::SetOrReportOffsets(GCodeBuffer &gb, const StringRef& reply, int code) THROWS(GCodeException)
{
	ReadLockedPointer<Tool> const tool = GetSpecifiedOrCurrentTool(gb);
	bool settingOffset = false;
	if (code == 10)			// Only G10 can set tool offsets
	{
		for (size_t axis = 0; axis < numVisibleAxes; ++axis)
		{
			if (gb.Seen(axisLetters[axis]))
			{
				if (!LockMovement(gb))
				{
					return GCodeResult::notFinished;
				}
				settingOffset = true;
				tool->SetOffset(axis, gb.GetFValue(), gb.LatestMachineState().runningM501);
			}
		}

		if (settingOffset)
		{
			ToolOffsetInverseTransform(moveState.coords, moveState.currentUserPosition);	// update user coordinates to reflect the new tool offset, in case we have this tool selected
		}
	}

	// Deal with setting temperatures
	bool settingTemps = false;
	size_t hCount = tool->HeaterCount();
	if (hCount > 0)
	{
		if (gb.Seen('R'))
		{
			settingTemps = true;
			if (!IsSimulating())
			{
				float standby[MaxHeaters];
				gb.GetFloatArray(standby, hCount, true);
				for (size_t h = 0; h < hCount; ++h)
				{
					tool->SetToolHeaterStandbyTemperature(h, standby[h]);
				}
			}
		}
		if (gb.Seen('S'))
		{
			settingTemps = true;
			if (!IsSimulating())
			{
				float activeTemps[MaxHeaters];
				gb.GetFloatArray(activeTemps, hCount, true);
				for (size_t h = 0; h < hCount; ++h)
				{
					tool->SetToolHeaterActiveTemperature(h, activeTemps[h]);		// may throw
				}
			}
		}
	}

	bool settingOther = false;
	if (code == 568)					// Only M568 can set spindle RPM and change tool heater states
	{
		// Deal with spindle RPM
		if (tool->GetSpindleNumber() > -1)
		{
			if (gb.Seen('F'))
			{
				settingOther = true;
				if (!IsSimulating())
				{
					tool->SetSpindleRpm(gb.GetUIValue());
				}
			}
		}

		// Deal with tool heater states
		uint32_t newHeaterState;
		if (gb.TryGetLimitedUIValue('A', newHeaterState, settingOther, 3))
		{
			switch (newHeaterState)
			{
			case 0:			// turn heaters off
				tool->HeatersToOff();
				break;

			case 1:			// turn heaters to standby, except any that are used by a different active tool
				tool->HeatersToActiveOrStandby(false);
				break;

			case 2:			// set heaters to their active temperatures, except any that are used by a different active tool
				tool->HeatersToActiveOrStandby(true);
				break;
			}
		}
	}

	if (!settingOffset && !settingTemps && !settingOther)
	{
		// Print offsets and temperatures
		reply.printf("Tool %d", tool->Number());
		char c;

		// Print the tool offsets if we are executing G10
		if (code == 10)
		{
			reply.cat(": offsets");
			for (size_t axis = 0; axis < numVisibleAxes; ++axis)
			{
				reply.catf(" %c%.3f", axisLetters[axis], (double)tool->GetOffset(axis));
			}
			c = ',';
		}
		else
		{
			c = ':';
		}

		// Print the heater active/standby temperatures whichever code we are executing
		if (hCount != 0)
		{
			reply.catf("%c active/standby temperature(s)", c);
			c = ',';
			for (size_t heater = 0; heater < hCount; heater++)
			{
				reply.catf(" %.1f/%.1f", (double)tool->GetToolHeaterActiveTemperature(heater), (double)tool->GetToolHeaterStandbyTemperature(heater));
			}
		}

		// Print the spindle number if we are executing M568
		if (code == 568 && tool->GetSpindleNumber() > -1)
		{
			reply.catf("%c spindle %d@%" PRIu32 "rpm", c, tool->GetSpindleNumber(), tool->GetSpindleRpm());
		}
	}
	else
	{
#if 0 // Do not warn about deprecation for now
		if (code == 10 && settingTemps)
		{
			reply.lcat("This use of G10 is deprecated. Please use M568 to set tool temperatures.");
			return GCodeResult::warning;
		}
#endif
		String<StringLengthLoggedCommand> scratch;
		gb.AppendFullCommand(scratch.GetRef());
		platform.Message(MessageType::LogInfo, scratch.c_str());
	}

	return GCodeResult::ok;
}

// Create a new tool definition
GCodeResult GCodes::ManageTool(GCodeBuffer& gb, const StringRef& reply)
{
	// Check tool number
	const unsigned int toolNumber = gb.GetLimitedUIValue('P', MaxTools);

	bool seen = false;

	// Check tool name
	String<ToolNameLength> name;
	if (gb.Seen('S'))
	{
		gb.GetQuotedString(name.GetRef());
		seen = true;
	}

	// Check drives
	int32_t drives[MaxExtrudersPerTool];
	size_t dCount = MaxExtrudersPerTool;	// Sets the limit and returns the count
	if (gb.Seen('D'))
	{
		gb.GetIntArray(drives, dCount, false);
		seen = true;
	}
	else
	{
		dCount = 0;
	}

	// Check heaters
	int32_t heaters[MaxHeatersPerTool];
	size_t hCount = MaxHeatersPerTool;
	if (gb.Seen('H'))
	{
		gb.GetIntArray(heaters, hCount, false);
		seen = true;
	}
	else
	{
		hCount = 0;
	}

	// Check X axis mapping
	AxesBitmap xMap;
	if (gb.Seen('X'))
	{
		uint32_t xMapping[MaxAxes];
		size_t xCount = numVisibleAxes;
		gb.GetUnsignedArray(xMapping, xCount, false);
		xMap = AxesBitmap::MakeFromArray(xMapping, xCount) & AxesBitmap::MakeLowestNBits(numVisibleAxes);
		seen = true;
	}
	else
	{
		xMap = DefaultXAxisMapping;					// by default map X axis straight through
	}

	// Check Y axis mapping
	AxesBitmap yMap;
	if (gb.Seen('Y'))
	{
		uint32_t yMapping[MaxAxes];
		size_t yCount = numVisibleAxes;
		gb.GetUnsignedArray(yMapping, yCount, false);
		yMap = AxesBitmap::MakeFromArray(yMapping, yCount) & AxesBitmap::MakeLowestNBits(numVisibleAxes);
		seen = true;
	}
	else
	{
		yMap = DefaultYAxisMapping;					// by default map X axis straight through
	}

	if (xMap.Intersects(yMap))
	{
		reply.copy("Cannot map both X and Y to the same axis");
		return GCodeResult::error;
	}

	// Check for fan mapping
	FansBitmap fanMap;
	if (gb.Seen('F'))
	{
		int32_t fanMapping[MaxFans];				// use a signed array so that F-1 will result in no fans at all
		size_t fanCount = MaxFans;
		gb.GetIntArray(fanMapping, fanCount, false);
		fanMap = FansBitmap::MakeFromArray(fanMapping, fanCount) & FansBitmap::MakeLowestNBits(MaxFans);
		seen = true;
	}
	else
	{
		fanMap.SetBit(0);							// by default map fan 0 to fan 0
	}

	// Check for a spindle to attach
	int8_t spindleNumber = -1;
	size_t sCount = 0;
	if (gb.Seen('R'))
	{
		seen = true;
		spindleNumber = gb.GetLimitedIValue('R', -1, MaxSpindles);
		++sCount;
	}

	if (seen)
	{
		if (!LockMovementAndWaitForStandstill(gb))
		{
			return GCodeResult::notFinished;
		}

		// Check if filament support is being enforced
		const int filamentDrive = (gb.Seen('L')) ? gb.GetIValue()
									: ((dCount == 1) ? drives[0]
										: -1);

		// Add or delete tool, so start by deleting the old one with this number, if any
		reprap.DeleteTool(toolNumber);

		// M563 P# D-1 H-1 [R-1] removes an existing tool
		if (dCount == 1 && hCount == 1 && drives[0] == -1 && heaters[0] == -1 && (sCount == 0 || (sCount == 1 && spindleNumber == -1)))
		{
			// nothing more to do
		}
		else
		{
			Tool* const tool = Tool::Create(toolNumber, name.c_str(), drives, dCount, heaters, hCount, xMap, yMap, fanMap, filamentDrive, sCount, spindleNumber, reply);
			if (tool == nullptr)
			{
				return GCodeResult::error;
			}
			reprap.AddTool(tool);
		}
	}
	else
	{
		reprap.PrintTool(toolNumber, reply);
	}
	return GCodeResult::ok;
}

// Does what it says.
void GCodes::DisableDrives() noexcept
{
	platform.DisableAllDrivers();
	SetAllAxesNotHomed();
}

bool GCodes::ChangeMicrostepping(size_t axisOrExtruder, unsigned int microsteps, bool interp, const StringRef& reply) const noexcept
{
	bool dummy;
	const unsigned int oldSteps = platform.GetMicrostepping(axisOrExtruder, dummy);
	const bool success = platform.SetMicrostepping(axisOrExtruder, microsteps, interp, reply);
	if (success)
	{
		// We changed the microstepping, so adjust the steps/mm to compensate
		platform.SetDriveStepsPerUnit(axisOrExtruder, platform.DriveStepsPerUnit(axisOrExtruder), oldSteps);
	}
	return success;
}

// Set the speeds of fans mapped for the current tool to lastDefaultFanSpeed
void GCodes::SetMappedFanSpeed(float f) noexcept
{
	lastDefaultFanSpeed = f;
	const Tool * const ct = reprap.GetCurrentTool();
	if (ct == nullptr)
	{
		reprap.GetFansManager().SetFanValue(0, f);
	}
	else
	{
		ct->SetFansPwm(f);
	}
}

// Return true if this fan number is currently being used as a print cooling fan
bool GCodes::IsMappedFan(unsigned int fanNumber) noexcept
{
	const Tool * const ct = reprap.GetCurrentTool();
	return (ct == nullptr)
			? fanNumber == 0
				: ct->GetFanMapping().IsBitSet(fanNumber);
}

// Handle sending a reply back to the appropriate interface(s) and update lastResult
// Note that 'reply' may be empty. If it isn't, then we need to append newline when sending it.
void GCodes::HandleReply(GCodeBuffer& gb, GCodeResult rslt, const char* reply) noexcept
{
	gb.SetLastResult(rslt);
	HandleReplyPreserveResult(gb, rslt, reply);
}

// Handle sending a reply back to the appropriate interface(s) but dpm't update lastResult
// Note that 'reply' may be empty. If it isn't, then we need to append newline when sending it.
void GCodes::HandleReplyPreserveResult(GCodeBuffer& gb, GCodeResult rslt, const char *reply) noexcept
{
#if HAS_SBC_INTERFACE
	// Deal with replies to the SBC
	if (gb.LatestMachineState().lastCodeFromSbc)
	{
		MessageType type = gb.GetResponseMessageType();
		if (rslt == GCodeResult::notFinished || gb.HasJustStartedMacro() ||
			(gb.LatestMachineState().waitingForAcknowledgement && gb.IsMessagePromptPending()))
		{
			if (reply[0] == 0)
			{
				// Don't send empty push messages
				return;
			}
			type = (MessageType)(type | PushFlag);
		}

		if (rslt == GCodeResult::warning)
		{
			type = AddWarning(type);
		}
		else if (rslt == GCodeResult::error)
		{
			type = AddError(type);
		}

		platform.Message(type, reply);
		return;
	}
#endif

	// Don't report empty responses if a file or macro is being processed, or if the GCode was queued, or to PanelDue
	if (   reply[0] == 0
		&& (   &gb == fileGCode || &gb == queuedGCode || &gb == triggerGCode || &gb == autoPauseGCode || &gb == daemonGCode
#if HAS_AUX_DEVICES
			|| (&gb == auxGCode && !platform.IsAuxRaw(0))
# ifdef SERIAL_AUX2_DEVICE
			|| (&gb == aux2GCode && !platform.IsAuxRaw(1))
# endif
#endif
			|| gb.IsDoingFileMacro()
		   )
	   )
	{
		return;
	}

	const MessageType initialMt = gb.GetResponseMessageType();
	const MessageType mt = (rslt == GCodeResult::error) ? AddError(initialMt)
							: (rslt == GCodeResult::warning) ? AddWarning(initialMt)
								: initialMt;

	switch (gb.LatestMachineState().compatibility.RawValue())
	{
	case Compatibility::Default:
	case Compatibility::RepRapFirmware:
		// DWC expects a reply from every code, so we must even send empty responses
		if (reply[0] != 0 || gb.IsLastCommand() || &gb == httpGCode)
		{
			platform.MessageF(mt, "%s\n", reply);
		}
		break;

	case Compatibility::NanoDLP:				// nanoDLP is like Marlin except that G0 and G1 commands return "Z_move_comp<LF>" before "ok<LF>"
	case Compatibility::Marlin:
		if (gb.IsLastCommand() && !gb.IsDoingFileMacro())
		{
			// Put "ok" at the end
			const char* const response = (gb.GetCommandLetter() == 'M' && gb.GetCommandNumber() == 998) ? "rs " : "ok";
			// We don't need to handle M20 here because we always allocate an output buffer for that one
			if (gb.GetCommandLetter() == 'M' && (gb.GetCommandNumber() == 105 || gb.GetCommandNumber() == 998))
			{
				platform.MessageF(mt, "%s %s\n", response, reply);
			}
			else if (gb.GetCommandLetter() == 'M' && gb.GetCommandNumber() == 28)
			{
				platform.MessageF(mt, "%s\n%s\n", response, reply);
			}
			else if (reply[0] != 0)
			{
				platform.MessageF(mt, "%s\n%s\n", reply, response);
			}
			else
			{
				platform.MessageF(mt, "%s\n", response);
			}
		}
		else if (reply[0] != 0)
		{
			platform.MessageF(mt, "%s\n", reply);
		}
		break;

	case Compatibility::Teacup:
	case Compatibility::Sprinter:
	case Compatibility::Repetier:
	default:
		platform.MessageF(mt, "Emulation of %s is not supported\n", gb.LatestMachineState().compatibility.ToString());
		break;
	}
}

// Handle a successful response when the response is in an OutputBuffer
void GCodes::HandleReply(GCodeBuffer& gb, OutputBuffer *reply) noexcept
{
	gb.SetLastResult(GCodeResult::ok);

	// Although unlikely, it's possible that we get a nullptr reply. Don't proceed if this is the case
	if (reply == nullptr)
	{
		return;
	}

#if HAS_SBC_INTERFACE
	// Deal with replies to the SBC
	if (gb.IsBinary())
	{
		platform.Message(gb.GetResponseMessageType(), reply);
		return;
	}
#endif

#if HAS_AUX_DEVICES
	// Second UART device, e.g. dc42's PanelDue. Do NOT use emulation for this one!
	if (&gb == auxGCode && !platform.IsAuxRaw(0))
	{
		platform.AppendAuxReply(0, reply, (*reply)[0] == '{');
		return;
	}
#endif

	const MessageType type = gb.GetResponseMessageType();
	const char* const response = (gb.GetCommandLetter() == 'M' && gb.GetCommandNumber() == 998) ? "rs " : "ok";

	switch (gb.LatestMachineState().compatibility.RawValue())
	{
	case Compatibility::Default:
	case Compatibility::RepRapFirmware:
		platform.Message(type, reply);
		return;

	case Compatibility::Marlin:
	case Compatibility::NanoDLP:
		if (gb.GetCommandLetter() == 'M')
		{
			// The response to some M-codes is handled differently in Marlin mode
			if (   gb.GetCommandNumber() == 20											// M20 in Marlin mode adds text around the file list
				&& ((*reply)[0] != '{' || (*reply)[1] != '"')							// ...but don't if it looks like a JSON response
			   )
			{
				platform.Message(type, "Begin file list\n");
				platform.Message(type, reply);
				platform.MessageF(type, "End file list\n%s\n", response);
				return;
			}

			if (gb.GetCommandNumber() == 28)
			{
				platform.MessageF(type, "%s\n", response);
				platform.Message(type, reply);
				return;
			}

			if (gb.GetCommandNumber() == 105 || gb.GetCommandNumber() == 998)
			{
				platform.MessageF(type, "%s ", response);
				platform.Message(type, reply);
				return;
			}
		}

		if (reply->Length() != 0)
		{
			platform.Message(type, reply);
			if (!gb.IsDoingFileMacro())
			{
				platform.MessageF(type, "\n%s\n", response);
			}
		}
		else
		{
			OutputBuffer::ReleaseAll(reply);
			platform.MessageF(type, "%s\n", response);
		}
		return;

	case Compatibility::Teacup:
	case Compatibility::Sprinter:
	case Compatibility::Repetier:
	default:
		platform.MessageF(type, "Emulation of %s is not supported\n", gb.LatestMachineState().compatibility.ToString());
		break;
	}

	// If we get here then we didn't handle the message, so release the buffer(s)
	OutputBuffer::ReleaseAll(reply);
}

void GCodes::SetToolHeaters(Tool *tool, float temperature, bool both) THROWS(GCodeException)
{
	if (tool == nullptr)
	{
		throw GCodeException(-1, -1, "setting temperature: no tool selected\n");
	}

	for (size_t h = 0; h < tool->HeaterCount(); h++)
	{
		tool->SetToolHeaterActiveTemperature(h, temperature);
		if (both)
		{
			tool->SetToolHeaterStandbyTemperature(h, temperature);
		}
	}
}

// Retract or un-retract filament, returning true if movement has been queued, false if this needs to be called again
GCodeResult GCodes::RetractFilament(GCodeBuffer& gb, bool retract)
{
	if (!buildObjects.IsCurrentObjectCancelled())
	{
		Tool* const currentTool = reprap.GetCurrentTool();
		if (  currentTool != nullptr
			&& retract != currentTool->IsRetracted()
			&& (currentTool->GetRetractLength() != 0.0 || currentTool->GetRetractHop() != 0.0 || (!retract && currentTool->GetRetractExtra() != 0.0))
		   )
		{
			if (!LockMovement(gb))
			{
				return GCodeResult::notFinished;
			}

			if (moveState.segmentsLeft != 0)
			{
				return GCodeResult::notFinished;
			}

			// New code does the retraction and the Z hop as separate moves
			// Get ready to generate a move
			SetMoveBufferDefaults();
			moveState.tool = reprap.GetCurrentTool();
			reprap.GetMove().GetCurrentUserPosition(moveState.coords, 0, moveState.tool);
			moveState.filePos = (&gb == fileGCode) ? gb.GetFilePosition() : noFilePosition;

			if (retract)
			{
				// Set up the retract move
				const Tool * const tool = reprap.GetCurrentTool();
				if (tool != nullptr && tool->DriveCount() != 0)
				{
					for (size_t i = 0; i < tool->DriveCount(); ++i)
					{
						moveState.coords[ExtruderToLogicalDrive(tool->GetDrive(i))] = -currentTool->GetRetractLength();
					}
					moveState.feedRate = currentTool->GetRetractSpeed() * tool->DriveCount();
					moveState.canPauseAfter = false;			// don't pause after a retraction because that could cause too much retraction
					NewSingleSegmentMoveAvailable();
				}
				if (currentTool->GetRetractHop() > 0.0)
				{
					gb.SetState(GCodeState::doingFirmwareRetraction);
				}
			}
			else if (moveState.currentZHop > 0.0)
			{
				// Set up the reverse Z hop move
				moveState.feedRate = platform.MaxFeedrate(Z_AXIS);
				moveState.coords[Z_AXIS] -= moveState.currentZHop;
				moveState.currentZHop = 0.0;
				moveState.canPauseAfter = false;			// don't pause in the middle of a command
				moveState.linearAxesMentioned = true;		// assume that both linear and rotational axes might be moving
				NewSingleSegmentMoveAvailable();
				gb.SetState(GCodeState::doingFirmwareUnRetraction);
			}
			else
			{
				// No retract hop, so just un-retract
				const Tool * const tool = reprap.GetCurrentTool();
				if (tool != nullptr && tool->DriveCount() != 0)
				{
					for (size_t i = 0; i < tool->DriveCount(); ++i)
					{
						moveState.coords[ExtruderToLogicalDrive(tool->GetDrive(i))] = currentTool->GetRetractLength() + currentTool->GetRetractExtra();
					}
					moveState.feedRate = currentTool->GetUnRetractSpeed() * tool->DriveCount();
					moveState.canPauseAfter = true;
					NewSingleSegmentMoveAvailable();
				}
			}
			currentTool->SetRetracted(retract);
		}
	}
	return GCodeResult::ok;
}

// Load the specified filament into a tool
GCodeResult GCodes::LoadFilament(GCodeBuffer& gb, const StringRef& reply) THROWS(GCodeException)
{
	Tool * const tool = reprap.GetCurrentTool();
	if (tool == nullptr)
	{
		reply.copy("No tool selected");
		return GCodeResult::error;
	}

	if (tool->GetFilament() == nullptr)
	{
		reply.copy("Loading filament into the selected tool is not supported");
		return GCodeResult::error;
	}

	if (gb.Seen('S'))
	{
		String<FilamentNameLength> filamentName;
		gb.GetQuotedString(filamentName.GetRef());

		if (filamentName.Contains(',') >= 0)
		{
			reply.copy("Filament names must not contain commas");
			return GCodeResult::error;
		}

		if (filamentName.EqualsIgnoreCase(tool->GetFilament()->GetName()))
		{
			// Filament already loaded - nothing to do
			return GCodeResult::ok;
		}

		if (tool->GetFilament()->IsLoaded())
		{
			reply.copy("Unload the current filament before you attempt to load another one");
			return GCodeResult::error;
		}

		SafeStrncpy(filamentToLoad, filamentName.c_str(), ARRAY_SIZE(filamentToLoad));
		gb.SetState(GCodeState::loadingFilament);

		String<StringLength256> scratchString;
		scratchString.printf("%s%s/%s", FILAMENTS_DIRECTORY, filamentName.c_str(), LOAD_FILAMENT_G);
		DoFileMacro(gb, scratchString.c_str(), true, SystemHelperMacroCode);
	}
	else if (tool->GetFilament()->IsLoaded())
	{
		reply.printf("Loaded filament in the selected tool: %s", tool->GetFilament()->GetName());
	}
	else
	{
		reply.printf("No filament loaded in the selected tool");
	}
	return GCodeResult::ok;
}

// Unload the current filament from a tool
GCodeResult GCodes::UnloadFilament(GCodeBuffer& gb, const StringRef& reply) THROWS(GCodeException)
{
	Tool * const tool = reprap.GetCurrentTool();
	if (tool == nullptr)
	{
		reply.copy("No tool selected");
		return GCodeResult::error;
	}

	if (tool->GetFilament() == nullptr)
	{
		reply.copy("Unloading filament from this tool is not supported");
		return GCodeResult::error;
	}

	if (tool->GetFilament()->IsLoaded())			// if no filament is loaded, nothing to do
	{
		gb.SetState(GCodeState::unloadingFilament);
		String<StringLength256> scratchString;
		scratchString.printf("%s%s/%s", FILAMENTS_DIRECTORY, tool->GetFilament()->GetName(), UNLOAD_FILAMENT_G);
		DoFileMacro(gb, scratchString.c_str(), true, SystemHelperMacroCode);
	}
	return GCodeResult::ok;
}

float GCodes::GetRawExtruderTotalByDrive(size_t extruder) const noexcept
{
	return (extruder < numExtruders) ? rawExtruderTotalByDrive[extruder] : 0.0;
}

// Return true if the code queue is idle
bool GCodes::IsCodeQueueIdle() const noexcept
{
	return queuedGCode->IsIdle() && codeQueue->IsIdle();
}

// Cancel the current SD card print.
// This is called from Pid.cpp when there is a heater fault, and from elsewhere in this module.
void GCodes::StopPrint(StopPrintReason reason) noexcept
{
	moveState.segmentsLeft = 0;
	deferredPauseCommandPending = nullptr;
	pauseState = PauseState::notPaused;

#if HAS_SBC_INTERFACE
	if (reprap.UsingSbcInterface())
	{
		fileGCode->ClosePrintFile();
		fileGCode->Init();
	}
	else
#endif
	{
#if HAS_MASS_STORAGE || HAS_EMBEDDED_FILES
		FileData& fileBeingPrinted = fileGCode->OriginalMachineState().fileState;

		fileGCode->GetFileInput()->Reset(fileBeingPrinted);
		fileGCode->Init();

		if (fileBeingPrinted.IsLive())
		{
			fileBeingPrinted.Close();
		}
#endif
	}

	// Don't call ReserMoveCounters here because we can't be sure that the movement queue is empty
	codeQueue->Clear();

	UnlockAll(*fileGCode);

	// Deal with the Z hop from a G10 that has not been undone by G11
	Tool* const currentTool = reprap.GetCurrentTool();
	if (currentTool != nullptr && currentTool->IsRetracted())
	{
		moveState.currentUserPosition[Z_AXIS] += moveState.currentZHop;
		moveState.currentZHop = 0.0;
		currentTool->SetRetracted(false);
	}

	const char *printingFilename = reprap.GetPrintMonitor().GetPrintingFilename();
	if (printingFilename == nullptr)
	{
		printingFilename = "(unknown)";
	}

	if (exitSimulationWhenFileComplete)
	{
		const float simSeconds = reprap.GetMove().GetSimulationTime() + simulationTime;
#if HAS_MASS_STORAGE
		if (updateFileWhenSimulationComplete && reason == StopPrintReason::normalCompletion)
		{
			MassStorage::RecordSimulationTime(printingFilename, lrintf(simSeconds));
		}
#endif

		exitSimulationWhenFileComplete = false;
		simulationMode = SimulationMode::off;				// do this after we append the simulation info to the file so that DWC doesn't try to reload the file info too soon
		reprap.GetMove().Simulate(simulationMode);
		EndSimulation(nullptr);

		const uint32_t simMinutes = lrintf(simSeconds/60.0);
		if (reason == StopPrintReason::normalCompletion)
		{
			lastDuration = simSeconds;
			platform.MessageF(LoggedGenericMessage, "File %s will print in %" PRIu32 "h %" PRIu32 "m plus heating time\n",
									printingFilename, simMinutes/60u, simMinutes % 60u);
		}
		else
		{
			lastDuration = 0;
			platform.MessageF(LoggedGenericMessage, "Cancelled simulating file %s after %" PRIu32 "h %" PRIu32 "m simulated time\n",
									printingFilename, simMinutes/60u, simMinutes % 60u);
		}
	}
	else if (reprap.GetPrintMonitor().IsPrinting())
	{
		if (reason == StopPrintReason::abort)
		{
			reprap.GetHeat().SwitchOffAll(true);	// turn all heaters off
			switch (machineType)
			{
			case MachineType::cnc:
				for (size_t i = 0; i < MaxSpindles; i++)
				{
					platform.AccessSpindle(i).SetState(SpindleState::stopped);
				}
				break;

#if SUPPORT_LASER
			case MachineType::laser:
				platform.SetLaserPwm(0);
				moveState.laserPwmOrIoBits.laserPwm = 0;
				break;
#endif

			default:
				break;
			}
		}

		// Pronterface expects a "Done printing" message
		if (usbGCode->LatestMachineState().compatibility == Compatibility::Marlin)
		{
			platform.Message(UsbMessage, "Done printing file\n");
		}
#if SUPPORT_TELNET
		if (telnetGCode->LatestMachineState().compatibility == Compatibility::Marlin)
		{
			platform.Message(TelnetMessage, "Done printing file\n");
		}
#endif
		const uint32_t printSeconds = lrintf(reprap.GetPrintMonitor().GetPrintDuration());
		const uint32_t printMinutes = printSeconds/60;
		lastDuration = (reason == StopPrintReason::normalCompletion) ? printSeconds : 0;
		platform.MessageF(LoggedGenericMessage, "%s printing file %s, print time was %" PRIu32 "h %" PRIu32 "m\n",
			(reason == StopPrintReason::normalCompletion) ? "Finished" : "Cancelled",
			printingFilename, printMinutes/60u, printMinutes % 60u);
#if HAS_MASS_STORAGE || HAS_SBC_INTERFACE
		if (reason == StopPrintReason::normalCompletion && !IsSimulating())
		{
			platform.DeleteSysFile(RESUME_AFTER_POWER_FAIL_G);
		}
#endif
	}

	updateFileWhenSimulationComplete = false;
	reprap.GetPrintMonitor().StoppedPrint();			// must do this after printing the simulation details not before, because it clears the filename and pause time
	buildObjects.Init();
	fileGCode->LatestMachineState().variables.Clear();	// delete any local variables that the file created
}

// Return true if all the heaters for the specified tool are at their set temperatures
bool GCodes::ToolHeatersAtSetTemperatures(const Tool *tool, bool waitWhenCooling, float tolerance) const noexcept
{
	if (tool != nullptr)
	{
		for (size_t i = 0; i < tool->HeaterCount(); ++i)
		{
			if (!reprap.GetHeat().HeaterAtSetTemperature(tool->GetHeater(i), waitWhenCooling, tolerance))
			{
				return false;
			}
		}
	}
	return true;
}

// Get the current position from the Move class
void GCodes::UpdateCurrentUserPosition(const GCodeBuffer& gb) noexcept
{
	reprap.GetMove().GetCurrentUserPosition(moveState.coords, 0, reprap.GetCurrentTool());
	ToolOffsetInverseTransform(moveState.coords, moveState.currentUserPosition);
#if SUPPORT_COORDINATE_ROTATION
	if (g68Angle != 0.0 && gb.DoingCoordinateRotation())
	{
		RotateCoordinates(-g68Angle, moveState.currentUserPosition);
	}
#endif
}

// Save position etc. to a restore point.
// Note that restore point coordinates are not affected by workplace coordinate offsets. This allows them to be used in resume.g.
void GCodes::SavePosition(RestorePoint& rp, const GCodeBuffer& gb) const noexcept
{
	for (size_t axis = 0; axis < numVisibleAxes; ++axis)
	{
		rp.moveCoords[axis] = moveState.currentUserPosition[axis];
	}

	rp.feedRate = gb.LatestMachineState().feedRate;
	rp.virtualExtruderPosition = virtualExtruderPosition;
	rp.filePos = gb.GetFilePosition();
	rp.toolNumber = reprap.GetCurrentToolNumber();
	rp.fanSpeed = lastDefaultFanSpeed;

#if SUPPORT_LASER || SUPPORT_IOBITS
	rp.laserPwmOrIoBits = moveState.laserPwmOrIoBits;
#endif
}

// Restore user position from a restore point. Also restore the laser power, but not the spindle speed (the user must do that explicitly).
void GCodes::RestorePosition(const RestorePoint& rp, GCodeBuffer *gb) noexcept
{
	for (size_t axis = 0; axis < numVisibleAxes; ++axis)
	{
		moveState.currentUserPosition[axis] = rp.moveCoords[axis];
	}

	if (gb != nullptr)
	{
		gb->LatestMachineState().feedRate = rp.feedRate;
	}

	moveState.initialUserC0 = rp.initialUserC0;
	moveState.initialUserC1 = rp.initialUserC1;

#if SUPPORT_LASER || SUPPORT_IOBITS
	moveState.laserPwmOrIoBits = rp.laserPwmOrIoBits;
#endif
}

// Convert user coordinates to head reference point coordinates, optionally allowing for X axis mapping
// If the X axis is mapped to some other axes not including X, then the X coordinate of coordsOut will be left unchanged.
// So make sure it is suitably initialised before calling this.
void GCodes::ToolOffsetTransform(const float coordsIn[MaxAxes], float coordsOut[MaxAxes], AxesBitmap explicitAxes) const noexcept
{
	const Tool * const currentTool = reprap.GetCurrentTool();
	if (currentTool == nullptr)
	{
		for (size_t axis = 0; axis < numVisibleAxes; ++axis)
		{
			coordsOut[axis] = (coordsIn[axis] * axisScaleFactors[axis]) + currentBabyStepOffsets[axis];
		}
	}
	else
	{
		const AxesBitmap xAxes = currentTool->GetXAxisMap();
		const AxesBitmap yAxes = currentTool->GetYAxisMap();
		for (size_t axis = 0; axis < numVisibleAxes; ++axis)
		{
			if (   (axis != X_AXIS || xAxes.IsBitSet(X_AXIS))
				&& (axis != Y_AXIS || yAxes.IsBitSet(Y_AXIS))
			   )
			{
				const float totalOffset = currentBabyStepOffsets[axis] - currentTool->GetOffset(axis);
				const size_t inputAxis = (explicitAxes.IsBitSet(axis)) ? axis
										: (xAxes.IsBitSet(axis)) ? X_AXIS
											: (yAxes.IsBitSet(axis)) ? Y_AXIS
												: axis;
				coordsOut[axis] = (coordsIn[inputAxis] * axisScaleFactors[axis]) + totalOffset;
			}
		}
	}
	coordsOut[Z_AXIS] += moveState.currentZHop;
}

// Convert head reference point coordinates to user coordinates, allowing for XY axis mapping
// Caution: coordsIn and coordsOut may address the same array!
void GCodes::ToolOffsetInverseTransform(const float coordsIn[MaxAxes], float coordsOut[MaxAxes]) const noexcept
{
	const Tool * const currentTool = reprap.GetCurrentTool();
	if (currentTool == nullptr)
	{
		for (size_t axis = 0; axis < numVisibleAxes; ++axis)
		{
			coordsOut[axis] = (coordsIn[axis] - currentBabyStepOffsets[axis])/axisScaleFactors[axis];
		}
	}
	else
	{
		const AxesBitmap xAxes = reprap.GetCurrentXAxes();
		const AxesBitmap yAxes = reprap.GetCurrentYAxes();
		float xCoord = 0.0, yCoord = 0.0;
		size_t numXAxes = 0, numYAxes = 0;
		for (size_t axis = 0; axis < numVisibleAxes; ++axis)
		{
			const float totalOffset = currentBabyStepOffsets[axis] - currentTool->GetOffset(axis);
			const float coord = (coordsIn[axis] - totalOffset)/axisScaleFactors[axis];
			coordsOut[axis] = coord;
			if (xAxes.IsBitSet(axis))
			{
				xCoord += coord;
				++numXAxes;
			}
			if (yAxes.IsBitSet(axis))
			{
				yCoord += coord;
				++numYAxes;
			}
		}
		if (numXAxes != 0)
		{
			coordsOut[X_AXIS] = xCoord/numXAxes;
		}
		if (numYAxes != 0)
		{
			coordsOut[Y_AXIS] = yCoord/numYAxes;
		}
	}
	coordsOut[Z_AXIS] -= moveState.currentZHop/axisScaleFactors[Z_AXIS];
}

// Get an axis offset of the current tool
float GCodes::GetCurrentToolOffset(size_t axis) const noexcept
{
	const Tool* const tool = reprap.GetCurrentTool();
	return (tool == nullptr) ? 0.0 : tool->GetOffset(axis);
}

// Get the current user coordinate and remove the coordinate rotation and workplace offset
float GCodes::GetUserCoordinate(size_t axis) const noexcept
{
	return (axis < numTotalAxes) ? moveState.currentUserPosition[axis] - GetWorkplaceOffset(axis) : 0.0;
}

#if HAS_MASS_STORAGE || HAS_EMBEDDED_FILES

// M38 (SHA1 hash of a file) implementation:
bool GCodes::StartHash(const char* filename) noexcept
{
	// Get a FileStore object
	fileBeingHashed = platform.OpenFile(FS_PREFIX, filename, OpenMode::read);
	if (fileBeingHashed == nullptr)
	{
		return false;
	}

	// Start hashing
	SHA1Reset(&hash);
	return true;
}

GCodeResult GCodes::AdvanceHash(const StringRef &reply) noexcept
{
	// Read and process some more data from the file
	alignas(4) char buffer[FILE_BUFFER_SIZE];
	const int bytesRead = fileBeingHashed->Read(buffer, FILE_BUFFER_SIZE);
	if (bytesRead != -1)
	{
		SHA1Input(&hash, reinterpret_cast<const uint8_t *>(buffer), bytesRead);

		if (bytesRead != FILE_BUFFER_SIZE)
		{
			// Calculate and report the final result
			SHA1Result(&hash);
			for (size_t i = 0; i < 5; i++)
			{
				reply.catf("%08" PRIx32, hash.Message_Digest[i]);
			}

			// Clean up again
			fileBeingHashed->Close();
			fileBeingHashed = nullptr;
			return GCodeResult::ok;
		}
		return GCodeResult::notFinished;
	}

	// Something went wrong, we cannot read any more from the file
	fileBeingHashed->Close();
	fileBeingHashed = nullptr;
	return GCodeResult::ok;
}

#endif

bool GCodes::AllAxesAreHomed() const noexcept
{
	const AxesBitmap allAxes = AxesBitmap::MakeLowestNBits(numVisibleAxes);
	return (axesVirtuallyHomed & allAxes) == allAxes;
}

// Tell us that the axis is now homed
void GCodes::SetAxisIsHomed(unsigned int axis) noexcept
{
	if (!IsSimulating())
	{
		axesHomed.SetBit(axis);
		axesVirtuallyHomed = axesHomed;
		reprap.MoveUpdated();
	}
}

// Tell us that the axis is not homed
void GCodes::SetAxisNotHomed(unsigned int axis) noexcept
{
	if (!IsSimulating())
	{
		axesHomed.ClearBit(axis);
		axesVirtuallyHomed = axesHomed;
		if (axis == Z_AXIS)
		{
			zDatumSetByProbing = false;
		}
		reprap.MoveUpdated();
	}
}

// Flag all axes as not homed
void GCodes::SetAllAxesNotHomed() noexcept
{
	if (!IsSimulating())
	{
		axesHomed.Clear();
		axesVirtuallyHomed = axesHomed;
		zDatumSetByProbing = false;
		reprap.MoveUpdated();
	}
}

#if HAS_MASS_STORAGE || HAS_SBC_INTERFACE

// Write the config-override file returning true if an error occurred
GCodeResult GCodes::WriteConfigOverrideFile(GCodeBuffer& gb, const StringRef& reply) const noexcept
{
	const char* const fileName = CONFIG_OVERRIDE_G;
	FileStore * const f = platform.OpenSysFile(fileName, OpenMode::write);
	if (f == nullptr)
	{
		reply.printf("Failed to create file %s", fileName);
		return GCodeResult::error;
	}

	bool ok = WriteConfigOverrideHeader(f);
	if (ok)
	{
		ok = reprap.GetMove().GetKinematics().WriteCalibrationParameters(f);
	}
	if (ok)
	{
		ok = reprap.GetHeat().WriteModelParameters(f);
	}

	// M500 can have a Pnn:nn parameter to enable extra data being saved
	// P10 will enable saving of tool offsets even if they have not been determined via M585
	bool p10 = false;

	// P31 will include G31 Z probe value
	bool p31 = false;
	if (gb.Seen('P'))
	{
		uint32_t pVals[2];
		size_t pCount = 2;
		gb.GetUnsignedArray(pVals, pCount, false);
		for (size_t i = 0; i < pCount; i++)
		{
			switch (pVals[i])
			{
			case 10:
				p10 = true;
				break;

			case 31:
				p31 = true;
				break;
			}
		}
	}
	if (ok)
	{
		ok = platform.WritePlatformParameters(f, p31);
	}

	if (ok)
	{
		ok = reprap.WriteToolParameters(f, p10);
	}

#if SUPPORT_WORKPLACE_COORDINATES
	if (ok)
	{
		ok = WriteWorkplaceCoordinates(f);
	}
#endif

	if (!f->Close())
	{
		ok = false;
	}

	if (!ok)
	{
		reply.printf("Failed to write file %s", fileName);
		platform.DeleteSysFile(fileName);
		return GCodeResult::error;
	}

	if (!m501SeenInConfigFile)
	{
		reply.copy("No M501 command was executed in config.g");
		return GCodeResult::warning;
	}

	return GCodeResult::ok;
}

// Write the config-override header returning true if success
// This is implemented as a separate function to avoid allocating a buffer on the stack and then calling functions that also allocate buffers on the stack
bool GCodes::WriteConfigOverrideHeader(FileStore *f) const noexcept
{
	String<MaxFilenameLength> buf;
	buf.copy("; config-override.g file generated in response to M500");
	tm timeInfo;
	if (platform.GetDateTime(timeInfo))
	{
		buf.catf(" at %04u-%02u-%02u %02u:%02u",
						timeInfo.tm_year + 1900, timeInfo.tm_mon + 1, timeInfo.tm_mday, timeInfo.tm_hour, timeInfo.tm_min);
	}
	buf.cat('\n');
	bool ok = f->Write(buf.c_str());
	if (ok)
	{
		ok = f->Write("; This is a system-generated file - do not edit\n");
	}
	return ok;
}

#endif

// Store a M105-format temperature report in 'reply'. This doesn't put a newline character at the end.
void GCodes::GenerateTemperatureReport(const StringRef& reply) const noexcept
{
	reprap.ReportAllToolTemperatures(reply);

	Heat& heat = reprap.GetHeat();
	for (size_t hn = 0; hn < MaxBedHeaters && heat.GetBedHeater(hn) >= 0; ++hn)
	{
		if (hn == 0)
		{
			if (reply.strlen() != 0)
			{
				reply.cat(' ');
			}
			reply.cat("B:");
		}
		else
		{
			reply.catf(" B%u:", hn);
		}
		const int8_t heater = heat.GetBedHeater(hn);
		reply.catf("%.1f /%.1f", (double)heat.GetHeaterTemperature(heater), (double)heat.GetTargetTemperature(heater));
	}

	for (size_t hn = 0; hn < MaxChamberHeaters && heat.GetChamberHeater(hn) >= 0; ++hn)
	{
		if (hn == 0)
		{
			if (reply.strlen() != 0)
			{
				reply.cat(' ');
			}
			reply.cat("C:");
		}
		else
		{
			reply.catf(" C%u:", hn);
		}
		const int8_t heater = heat.GetChamberHeater(hn);
		reply.catf("%.1f /%.1f", (double)heat.GetHeaterTemperature(heater), (double)heat.GetTargetTemperature(heater));
	}
}

// Check whether we need to report temperatures or status.
// 'reply' is a convenient buffer that is free for us to use.
void GCodes::CheckReportDue(GCodeBuffer& gb, const StringRef& reply) const noexcept
{
	if (&gb == usbGCode)
	{
		if (gb.LatestMachineState().compatibility == Compatibility::Marlin && gb.IsReportDue())
		{
			// In Marlin emulation mode we should return a standard temperature report every second
			GenerateTemperatureReport(reply);
			if (reply.strlen() > 0)
			{
				reply.cat('\n');
				platform.Message(UsbMessage, reply.c_str());
				reply.Clear();
			}
		}
	}
	else if (&gb == auxGCode)
	{
		if (lastAuxStatusReportType >= 0 && platform.IsAuxEnabled(0) && gb.IsReportDue())
		{
			// Send a standard status response for PanelDue
			OutputBuffer * const statusBuf =
									(lastAuxStatusReportType == ObjectModelAuxStatusReportType)		// PanelDueFirmware v3.2 or later, using M409 to retrieve object model
										? reprap.GetModelResponse(&gb, "", "d99fi")
										: GenerateJsonStatusResponse(lastAuxStatusReportType, -1, ResponseSource::AUX);		// older PanelDueFirmware using M408
			if (statusBuf != nullptr)
			{
				platform.AppendAuxReply(0, statusBuf, true);
				if (reprap.Debug(moduleGcodes))
				{
					debugPrintf("%s: Sent unsolicited status report\n", gb.GetChannel().ToString());
				}
			}
		}
	}
}

// Generate a M408 response
// Return the output buffer containing the response, or nullptr if we failed
OutputBuffer *GCodes::GenerateJsonStatusResponse(int type, int seq, ResponseSource source) const noexcept
{
	OutputBuffer *statusResponse = nullptr;
	switch (type)
	{
		case 0:
		case 1:
			statusResponse = reprap.GetLegacyStatusResponse(type + 2, seq);
			break;

		default:				// need a default clause to prevent the command hanging by always returning a null buffer
			type = 2;
			// no break
		case 2:
		case 3:
		case 4:
			statusResponse = reprap.GetStatusResponse(type - 1, source);
			break;

		case 5:
			statusResponse = reprap.GetConfigResponse();
			break;
	}
	if (statusResponse != nullptr)
	{
		statusResponse->cat('\n');
		if (statusResponse->HadOverflow())
		{
			OutputBuffer::ReleaseAll(statusResponse);
		}
	}
	return statusResponse;
}

// Initiate a tool change. Caller has already checked that the correct tool isn't loaded.
void GCodes::StartToolChange(GCodeBuffer& gb, int toolNum, uint8_t param) noexcept
{
	newToolNumber = toolNum;
	toolChangeParam = (IsSimulating()) ? 0 : param;
	gb.SetState(GCodeState::toolChange0);
}

// Set up some default values in the move buffer for special moves, e.g. for Z probing and firmware retraction
void GCodes::SetMoveBufferDefaults() noexcept
{
	moveState.SetDefaults(numTotalAxes);
}

// Resource locking/unlocking

// Lock the resource, returning true if success.
// Locking the same resource more than once only locks it once, there is no lock count held.
bool GCodes::LockResource(const GCodeBuffer& gb, Resource r) noexcept
{
	TaskCriticalSectionLocker lock;

	if (resourceOwners[r] == &gb)
	{
		return true;
	}
	if (resourceOwners[r] == nullptr)
	{
		resourceOwners[r] = &gb;
		gb.LatestMachineState().lockedResources.SetBit(r);
		return true;
	}
	return false;
}

// Grab the movement lock even if another GCode source has it
void GCodes::GrabResource(const GCodeBuffer& gb, Resource r) noexcept
{
	TaskCriticalSectionLocker lock;

	if (resourceOwners[r] != &gb)
	{
		// Note, we now leave the resource bit set in the original owning GCodeBuffer machine state
		resourceOwners[r] = &gb;
		gb.LatestMachineState().lockedResources.SetBit(r);
	}
}

// Lock the unshareable parts of the file system
bool GCodes::LockFileSystem(const GCodeBuffer &gb) noexcept
{
	return LockResource(gb, FileSystemResource);
}

// Lock movement
bool GCodes::LockMovement(const GCodeBuffer& gb) noexcept
{
	return LockResource(gb, MoveResource);
}

// Grab the movement lock even if another channel owns it
void GCodes::GrabMovement(const GCodeBuffer& gb) noexcept
{
	GrabResource(gb, MoveResource);
}

// Release the movement lock
void GCodes::UnlockMovement(const GCodeBuffer& gb) noexcept
{
	UnlockResource(gb, MoveResource);
}

// Unlock the resource if we own it
void GCodes::UnlockResource(const GCodeBuffer& gb, Resource r) noexcept
{
	TaskCriticalSectionLocker lock;

	if (resourceOwners[r] == &gb)
	{
		// Note, we leave the bit set in previous stack levels! This is needed e.g. to allow M291 blocking messages to be used in homing files.
		gb.LatestMachineState().lockedResources.ClearBit(r);
		resourceOwners[r] = nullptr;
	}
}

// Release all locks, except those that were owned when the current macro was started
void GCodes::UnlockAll(const GCodeBuffer& gb) noexcept
{
	TaskCriticalSectionLocker lock;

	const GCodeMachineState * const mc = gb.LatestMachineState().GetPrevious();
	const GCodeMachineState::ResourceBitmap resourcesToKeep = (mc == nullptr) ? GCodeMachineState::ResourceBitmap() : mc->lockedResources;
	for (size_t i = 0; i < NumResources; ++i)
	{
		if (resourceOwners[i] == &gb && !resourcesToKeep.IsBitSet(i))
		{
			if (i == MoveResource)
			{
				// In case homing was aborted because of an exception, we need to clear toBeHomed when releasing the movement lock
				toBeHomed.Clear();
			}
			resourceOwners[i] = nullptr;
			gb.LatestMachineState().lockedResources.ClearBit(i);
		}
	}
}

// Append a list of axes to a string
void GCodes::AppendAxes(const StringRef& reply, AxesBitmap axes) const noexcept
{
	axes.Iterate([&reply, this](unsigned int axis, unsigned int) noexcept { reply.cat(this->axisLetters[axis]); });
}

// Get the name of the current machine mode
const char* GCodes::GetMachineModeString() const noexcept
{
	switch (machineType)
	{
	case MachineType::fff:
		return "FFF";
	case MachineType::cnc:
		return "CNC";
	case MachineType::laser:
		return "Laser";
	default:
		return "Unknown";
	}
}

// Return a current extrusion factor as a fraction
float GCodes::GetExtrusionFactor(size_t extruder) noexcept
{
	return (extruder < numExtruders) ? extrusionFactors[extruder] : 0.0;
}

size_t GCodes::GetAxisNumberForLetter(const char axisLetter) const noexcept
{
	for (size_t i = 0; i < numTotalAxes; ++i)
	{
		if (axisLetters[i] == axisLetter)
		{
			return i;
		}
	}
	return MaxAxes;
}

Pwm_t GCodes::ConvertLaserPwm(float reqVal) const noexcept
{
	return (uint16_t)constrain<long>(lrintf((reqVal * 65535)/laserMaxPower), 0, 65535);
}

void GCodes::ActivateHeightmap(bool activate) noexcept
{
	reprap.GetMove().UseMesh(activate);
	if (activate)
	{
		// Update the current position to allow for any bed compensation at the current XY coordinates
		reprap.GetMove().GetCurrentUserPosition(moveState.coords, 0, reprap.GetCurrentTool());
		ToolOffsetInverseTransform(moveState.coords, moveState.currentUserPosition);	// update user coordinates to reflect any height map offset at the current position
	}
}

// Check that we are allowed to perform network-related commands
// Return true if we are; else return false and set 'reply' and 'result' appropriately
// On entry, 'reply' is empty and 'result' is GCodeResult::ok
bool GCodes::CheckNetworkCommandAllowed(GCodeBuffer& gb, const StringRef& reply, GCodeResult& result) noexcept
{
	if (gb.LatestMachineState().runningM502)			// when running M502 we don't execute network-related commands
	{
		return false;									// just ignore the command but report success
	}

#if HAS_SBC_INTERFACE
	if (reprap.UsingSbcInterface())
	{
		// Networking is disabled when using the SBC interface, to save RAM
		reply.copy("Network-related commands are not supported when using an attached Single Board Computer");
		result = GCodeResult::error;
		return false;
	}
#endif

	return true;
}

#if HAS_MASS_STORAGE

// Start timing SD card file writing
GCodeResult GCodes::StartSDTiming(GCodeBuffer& gb, const StringRef& reply) noexcept
{
	const float bytesReq = (gb.Seen('S')) ? gb.GetFValue() : 10.0;
	const bool useCrc = (gb.Seen('C') && gb.GetUIValue() != 0);
	timingBytesRequested = (uint32_t)(bytesReq * (float)(1024 * 1024));
	FileStore * const f = platform.OpenFile(Platform::GetGCodeDir(), TimingFileName, (useCrc) ? OpenMode::writeWithCrc : OpenMode::write, timingBytesRequested);
	if (f == nullptr)
	{
		reply.copy("Failed to create file");
		return GCodeResult::error;
	}
	sdTimingFile = f;

	platform.Message(gb.GetResponseMessageType(), "Testing SD card write speed...\n");
	timingBytesWritten = 0;
	timingStartMillis = millis();
	gb.SetState(GCodeState::timingSDwrite);
	return GCodeResult::ok;
}

#endif

#if SUPPORT_12864_LCD

// Set the speed factor. Value passed is a fraction.
void GCodes::SetSpeedFactor(float factor) noexcept
{
	speedFactor = constrain<float>(factor, 0.1, 5.0);
}

// Set an extrusion factor
void GCodes::SetExtrusionFactor(size_t extruder, float factor) noexcept
{
	if (extruder < numExtruders)
	{
		extrusionFactors[extruder] = constrain<float>(factor, 0.0, 2.0);
	}
}

// Process a GCode command from the 12864 LCD returning true if the command was accepted
bool GCodes::ProcessCommandFromLcd(const char *cmd) noexcept
{
	if (lcdGCode->IsCompletelyIdle())
	{
		lcdGCode->PutAndDecode(cmd);
		return true;
	}
	return false;
}

int GCodes::GetHeaterNumber(unsigned int itemNumber) const noexcept
{
	if (itemNumber < 80)
	{
		ReadLockedPointer<Tool> const tool = (itemNumber == 79) ? reprap.GetLockedCurrentTool() : reprap.GetTool(itemNumber);
		return (tool.IsNotNull() && tool->HeaterCount() != 0) ? tool->GetHeater(0) : -1;
	}
	if (itemNumber < 90)
	{
		return (itemNumber < 80 + MaxBedHeaters) ? reprap.GetHeat().GetBedHeater(itemNumber - 80) : -1;
	}
	return (itemNumber < 90 + MaxChamberHeaters) ? reprap.GetHeat().GetChamberHeater(itemNumber - 90) : -1;
}

float GCodes::GetItemCurrentTemperature(unsigned int itemNumber) const noexcept
{
	return reprap.GetHeat().GetHeaterTemperature(GetHeaterNumber(itemNumber));
}

float GCodes::GetItemActiveTemperature(unsigned int itemNumber) const noexcept
{
	if (itemNumber < 80)
	{
		ReadLockedPointer<Tool> const tool = (itemNumber == 79) ? reprap.GetLockedCurrentTool() : reprap.GetTool(itemNumber);
		return (tool.IsNotNull()) ? tool->GetToolHeaterActiveTemperature(0) : 0.0;
	}

	return reprap.GetHeat().GetActiveTemperature(GetHeaterNumber(itemNumber));
}

float GCodes::GetItemStandbyTemperature(unsigned int itemNumber) const noexcept
{
	if (itemNumber < 80)
	{
		ReadLockedPointer<Tool> const tool = (itemNumber == 79) ? reprap.GetLockedCurrentTool() : reprap.GetTool(itemNumber);
		return (tool.IsNotNull()) ? tool->GetToolHeaterStandbyTemperature(0) : 0.0;
	}

	return reprap.GetHeat().GetStandbyTemperature(GetHeaterNumber(itemNumber));
}

void GCodes::SetItemActiveTemperature(unsigned int itemNumber, float temp) noexcept
{
	if (itemNumber < 80)
	{
		ReadLockedPointer<Tool> const tool = (itemNumber == 79) ? reprap.GetLockedCurrentTool() : reprap.GetTool(itemNumber);
		if (tool.IsNotNull())
		{
			tool->SetToolHeaterActiveTemperature(0, temp);
			if (tool->Number() == reprap.GetCurrentToolNumber() && temp > NEARLY_ABS_ZERO)
			{
				tool->HeatersToActiveOrStandby(true);				// if it's the current tool then make sure it is active
			}
		}
	}
	else
	{
		const int heaterNumber = GetHeaterNumber(itemNumber);
		reprap.GetHeat().SetActiveTemperature(heaterNumber, temp);
		if (temp > NEARLY_ABS_ZERO)
		{
			String<1> dummy;
			reprap.GetHeat().SetActiveOrStandby(heaterNumber, nullptr, true, dummy.GetRef());
		}
	}
}

void GCodes::SetItemStandbyTemperature(unsigned int itemNumber, float temp) noexcept
{
	if (itemNumber < 80)
	{
		ReadLockedPointer<Tool> const tool = (itemNumber == 79) ? reprap.GetLockedCurrentTool() : reprap.GetTool(itemNumber);
		if (tool.IsNotNull())
		{
			tool->SetToolHeaterStandbyTemperature(0, temp);
		}
	}
	else
	{
		reprap.GetHeat().SetStandbyTemperature(GetHeaterNumber(itemNumber), temp);
	}
}

#endif

// End
