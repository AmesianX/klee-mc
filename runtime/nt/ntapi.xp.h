#ifndef WINNT_NT_xp
#define WINNT_NT_xp
#define NtAcceptConnectPort 0x0000
#define NtAccessCheck 0x0001
#define NtAccessCheckAndAuditAlarm 0x0002
#define NtAccessCheckByType 0x0003
#define NtAccessCheckByTypeAndAuditAlarm 0x0004
#define NtAccessCheckByTypeResultList 0x0005
#define NtAccessCheckByTypeResultListAndAuditAlarm 0x0006
#define NtAccessCheckByTypeResultListAndAuditAlarmByHandle 0x0007
#define NtAddAtom 0x0008
#define NtAdjustGroupsToken 0x000a
#define NtAdjustPrivilegesToken 0x000b
#define NtAlertResumeThread 0x000c
#define NtAlertThread 0x000d
#define NtAllocateLocallyUniqueId 0x000e
#define NtAllocateUserPhysicalPages 0x000f
#define NtAllocateUuids 0x0010
#define NtAllocateVirtualMemory 0x0011
#define NtAreMappedFilesTheSame 0x0012
#define NtAssignProcessToJobObject 0x0013
#define NtCallbackReturn 0x0014
#define NtCancelIoFile 0x0016
#define NtCancelTimer 0x0017
#define NtClearEvent 0x0018
#define NtClose 0x0019
#define NtCloseObjectAuditAlarm 0x001a
#define NtCompactKeys 0x001b
#define NtCompareTokens 0x001c
#define NtCompleteConnectPort 0x001d
#define NtCompressKey 0x001e
#define NtConnectPort 0x001f
#define NtContinue 0x0020
#define NtCreateDebugObject 0x0021
#define NtCreateDirectoryObject 0x0022
#define NtCreateEvent 0x0023
#define NtCreateEventPair 0x0024
#define NtCreateFile 0x0025
#define NtCreateIoCompletion 0x0026
#define NtCreateJobObject 0x0027
#define NtCreateJobSet 0x0028
#define NtCreateKey 0x0029
#define NtCreateKeyedEvent 0x0117
#define NtCreateMailslotFile 0x002a
#define NtCreateMutant 0x002b
#define NtCreateNamedPipeFile 0x002c
#define NtCreatePagingFile 0x002d
#define NtCreatePort 0x002e
#define NtCreateProcess 0x002f
#define NtCreateProcessEx 0x0030
#define NtCreateProfile 0x0031
#define NtCreateSection 0x0032
#define NtCreateSemaphore 0x0033
#define NtCreateSymbolicLinkObject 0x0034
#define NtCreateThread 0x0035
#define NtCreateTimer 0x0036
#define NtCreateToken 0x0037
#define NtCreateWaitablePort 0x0038
#define NtDebugActiveProcess 0x0039
#define NtDebugContinue 0x003a
#define NtDelayExecution 0x003b
#define NtDeleteAtom 0x003c
#define NtDeleteFile 0x003e
#define NtDeleteKey 0x003f
#define NtDeleteObjectAuditAlarm 0x0040
#define NtDeleteValueKey 0x0041
#define NtDeviceIoControlFile 0x0042
#define NtDisplayString 0x0043
#define NtDuplicateObject 0x0044
#define NtDuplicateToken 0x0045
#define NtEnumerateBootEntries 0x0009
#define NtEnumerateKey 0x0047
#define NtEnumerateSystemEnvironmentValuesEx 0x0048
#define NtEnumerateValueKey 0x0049
#define NtExtendSection 0x004a
#define NtFilterToken 0x004b
#define NtFindAtom 0x004c
#define NtFlushBuffersFile 0x004d
#define NtFlushInstructionCache 0x004e
#define NtFlushKey 0x004f
#define NtFlushVirtualMemory 0x0050
#define NtFlushWriteBuffer 0x0051
#define NtFreeUserPhysicalPages 0x0052
#define NtFreeVirtualMemory 0x0053
#define NtFsControlFile 0x0054
#define NtGetContextThread 0x0055
#define NtGetDevicePowerState 0x0056
#define NtGetPlugPlayEvent 0x0057
#define NtGetWriteWatch 0x0058
#define NtImpersonateAnonymousToken 0x0059
#define NtImpersonateClientOfPort 0x005a
#define NtImpersonateThread 0x005b
#define NtInitializeRegistry 0x005c
#define NtInitiatePowerAction 0x005d
#define NtIsProcessInJob 0x005e
#define NtIsSystemResumeAutomatic 0x005f
#define NtListenPort 0x0060
#define NtLoadDriver 0x0061
#define NtLoadKey 0x0062
#define NtLoadKey2 0x0063
#define NtLockFile 0x0064
#define NtLockProductActivationKeys 0x0065
#define NtLockRegistryKey 0x0066
#define NtLockVirtualMemory 0x0067
#define NtMakePermanentObject 0x0068
#define NtMakeTemporaryObject 0x0069
#define NtMapUserPhysicalPages 0x006a
#define NtMapUserPhysicalPagesScatter 0x006b
#define NtMapViewOfSection 0x006c
#define NtModifyBootEntry 0x0015
#define NtNotifyChangeDirectoryFile 0x006e
#define NtNotifyChangeKey 0x006f
#define NtNotifyChangeMultipleKeys 0x0070
#define NtOpenDirectoryObject 0x0071
#define NtOpenEvent 0x0072
#define NtOpenEventPair 0x0073
#define NtOpenFile 0x0074
#define NtOpenIoCompletion 0x0075
#define NtOpenJobObject 0x0076
#define NtOpenKey 0x0077
#define NtOpenKeyedEvent 0x0118
#define NtOpenMutant 0x0078
#define NtOpenObjectAuditAlarm 0x0079
#define NtOpenProcess 0x007a
#define NtOpenProcessToken 0x007b
#define NtOpenProcessTokenEx 0x007c
#define NtOpenSection 0x007d
#define NtOpenSemaphore 0x007e
#define NtOpenSymbolicLinkObject 0x007f
#define NtOpenThread 0x0080
#define NtOpenThreadToken 0x0081
#define NtOpenThreadTokenEx 0x0082
#define NtOpenTimer 0x0083
#define NtPlugPlayControl 0x0084
#define NtPowerInformation 0x0085
#define NtPrivilegeCheck 0x0086
#define NtPrivilegeObjectAuditAlarm 0x0087
#define NtPrivilegedServiceAuditAlarm 0x0088
#define NtProtectVirtualMemory 0x0089
#define NtPulseEvent 0x008a
#define NtQueryAttributesFile 0x008b
#define NtQueryDebugFilterState 0x008e
#define NtQueryDefaultLocale 0x008f
#define NtQueryDefaultUILanguage 0x0090
#define NtQueryDirectoryFile 0x0091
#define NtQueryDirectoryObject 0x0092
#define NtQueryEaFile 0x0093
#define NtQueryEvent 0x0094
#define NtQueryFullAttributesFile 0x0095
#define NtQueryInformationAtom 0x0096
#define NtQueryInformationFile 0x0097
#define NtQueryInformationJobObject 0x0098
#define NtQueryInformationPort 0x0099
#define NtQueryInformationProcess 0x009a
#define NtQueryInformationThread 0x009b
#define NtQueryInformationToken 0x009c
#define NtQueryInstallUILanguage 0x009d
#define NtQueryIntervalProfile 0x009e
#define NtQueryIoCompletion 0x009f
#define NtQueryKey 0x00a0
#define NtQueryMultipleValueKey 0x00a1
#define NtQueryMutant 0x00a2
#define NtQueryObject 0x00a3
#define NtQueryOpenSubKeys 0x00a4
#define NtQueryPerformanceCounter 0x00a5
#define NtQueryPortInformationProcess 0x011b
#define NtQueryQuotaInformationFile 0x00a6
#define NtQuerySection 0x00a7
#define NtQuerySecurityObject 0x00a8
#define NtQuerySemaphore 0x00a9
#define NtQuerySymbolicLinkObject 0x00aa
#define NtQuerySystemEnvironmentValue 0x00ab
#define NtQuerySystemEnvironmentValueEx 0x00ac
#define NtQuerySystemInformation 0x00ad
#define NtQuerySystemTime 0x00ae
#define NtQueryTimer 0x00af
#define NtQueryTimerResolution 0x00b0
#define NtQueryValueKey 0x00b1
#define NtQueryVirtualMemory 0x00b2
#define NtQueryVolumeInformationFile 0x00b3
#define NtQueueApcThread 0x00b4
#define NtRaiseException 0x00b5
#define NtRaiseHardError 0x00b6
#define NtReadFile 0x00b7
#define NtReadFileScatter 0x00b8
#define NtReadRequestData 0x00b9
#define NtReadVirtualMemory 0x00ba
#define NtRegisterThreadTerminatePort 0x00bb
#define NtReleaseKeyedEvent 0x0119
#define NtReleaseMutant 0x00bc
#define NtReleaseSemaphore 0x00bd
#define NtRemoveIoCompletion 0x00be
#define NtRemoveProcessDebug 0x00bf
#define NtRenameKey 0x00c0
#define NtReplaceKey 0x00c1
#define NtReplyPort 0x00c2
#define NtReplyWaitReceivePort 0x00c3
#define NtReplyWaitReceivePortEx 0x00c4
#define NtReplyWaitReplyPort 0x00c5
#define NtRequestDeviceWakeup 0x00c6
#define NtRequestPort 0x00c7
#define NtRequestWaitReplyPort 0x00c8
#define NtRequestWakeupLatency 0x00c9
#define NtResetEvent 0x00ca
#define NtResetWriteWatch 0x00cb
#define NtRestoreKey 0x00cc
#define NtResumeProcess 0x00cd
#define NtResumeThread 0x00ce
#define NtSaveKey 0x00cf
#define NtSaveKeyEx 0x00d0
#define NtSaveMergedKeys 0x00d1
#define NtSecureConnectPort 0x00d2
#define NtSetContextThread 0x00d5
#define NtSetDebugFilterState 0x00d6
#define NtSetDefaultHardErrorPort 0x00d7
#define NtSetDefaultLocale 0x00d8
#define NtSetDefaultUILanguage 0x00d9
#define NtSetEaFile 0x00da
#define NtSetEvent 0x00db
#define NtSetEventBoostPriority 0x00dc
#define NtSetHighEventPair 0x00dd
#define NtSetHighWaitLowEventPair 0x00de
#define NtSetInformationDebugObject 0x00df
#define NtSetInformationFile 0x00e0
#define NtSetInformationJobObject 0x00e1
#define NtSetInformationKey 0x00e2
#define NtSetInformationObject 0x00e3
#define NtSetInformationProcess 0x00e4
#define NtSetInformationThread 0x00e5
#define NtSetInformationToken 0x00e6
#define NtSetIntervalProfile 0x00e7
#define NtSetIoCompletion 0x00e8
#define NtSetLdtEntries 0x00e9
#define NtSetLowEventPair 0x00ea
#define NtSetLowWaitHighEventPair 0x00eb
#define NtSetQuotaInformationFile 0x00ec
#define NtSetSecurityObject 0x00ed
#define NtSetSystemEnvironmentValue 0x00ee
#define NtSetSystemInformation 0x00f0
#define NtSetSystemPowerState 0x00f1
#define NtSetSystemTime 0x00f2
#define NtSetThreadExecutionState 0x00f3
#define NtSetTimer 0x00f4
#define NtSetTimerResolution 0x00f5
#define NtSetUuidSeed 0x00f6
#define NtSetValueKey 0x00f7
#define NtSetVolumeInformationFile 0x00f8
#define NtShutdownSystem 0x00f9
#define NtSignalAndWaitForSingleObject 0x00fa
#define NtStartProfile 0x00fb
#define NtStopProfile 0x00fc
#define NtSuspendProcess 0x00fd
#define NtSuspendThread 0x00fe
#define NtSystemDebugControl 0x00ff
#define NtTerminateJobObject 0x0100
#define NtTerminateProcess 0x0101
#define NtTerminateThread 0x0102
#define NtTestAlert 0x0103
#define NtTraceEvent 0x0104
#define NtTranslateFilePath 0x0105
#define NtUnloadDriver 0x0106
#define NtUnloadKey 0x0107
#define NtUnloadKeyEx 0x0108
#define NtUnlockFile 0x0109
#define NtUnlockVirtualMemory 0x010a
#define NtUnmapViewOfSection 0x010b
#define NtVdmControl 0x010c
#define NtWaitForDebugEvent 0x010d
#define NtWaitForKeyedEvent 0x011a
#define NtWaitForMultipleObjects 0x010e
#define NtWaitForSingleObject 0x010f
#define NtWaitHighEventPair 0x0110
#define NtWaitLowEventPair 0x0111
#define NtWriteFile 0x0112
#define NtWriteFileGather 0x0113
#define NtWriteRequestData 0x0114
#define NtWriteVirtualMemory 0x0115
#define NtYieldExecution 0x0116
#endif
