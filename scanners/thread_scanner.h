#pragma once

#include <windows.h>

#include "module_scanner.h"
#include "../utils/threads_util.h"
#include "../utils/process_symbols.h"
#include "../stats/stats.h"
#include "../stats/entropy_stats.h"

namespace pesieve {

	typedef enum ThSusIndicator {
		THI_NONE,
		THI_SUS_START,
		THI_SUS_IP,
		THI_SUS_RET,
		THI_SUS_CALLSTACK_SHC,
		THI_SUS_CALLS_INTEGRITY,
		THI_SUS_CALLSTACK_CORRUPT,
		THI_MAX
	} _ThSusIndicator;

	inline std::string indicator_to_str(const ThSusIndicator& indicator)
	{
		switch (indicator) {
			case THI_NONE: return "NONE";
			case THI_SUS_START: return "SUS_START";
			case THI_SUS_IP: return "SUS_IP";
			case THI_SUS_RET: return "SUS_RET";
			case THI_SUS_CALLSTACK_SHC: return "SUS_CALLSTACK_SHC";
			case THI_SUS_CALLS_INTEGRITY: return "SUS_CALLS_INTEGRITY";
			case THI_SUS_CALLSTACK_CORRUPT: return "SUS_CALLSTACK_CORRUPT";
		}
		return "";
	}

	//!  A report from the thread scan, generated by ThreadScanner
	class ThreadScanReport : public ModuleScanReport
	{
	public:
		static const DWORD THREAD_STATE_UNKNOWN = (-1);
		static const DWORD THREAD_STATE_WAITING = 5;

		static std::string translate_thread_state(DWORD thread_state);
		static std::string translate_wait_reason(DWORD thread_wait_reason);
		
		//---

		ThreadScanReport(DWORD _tid)
			: ModuleScanReport(0, 0), 
			tid(_tid), 
			susp_addr(0), protection(0), stack_ptr(0), frames_count(0),
			thread_state(THREAD_STATE_UNKNOWN), 
			thread_wait_reason(0), thread_wait_time(0), is_code(false)
		{
		}

		const virtual void fieldsToJSON(std::stringstream &outs, size_t level, const pesieve::t_json_level &jdetails)
		{
			ModuleScanReport::_toJSON(outs, level);
			outs << ",\n";
			OUT_PADDED(outs, level, "\"thread_id\" : ");
			outs << std::dec << tid;

			outs << ",\n";
			OUT_PADDED(outs, level, "\"indicators\" : [");
			for (auto itr = indicators.begin(); itr != indicators.end(); ++itr) {
				if (itr != indicators.begin()) {
					outs << ", ";
				}
				outs << "\"" << indicator_to_str(*itr) << "\"";
			}
			outs << "]";
			if (stack_ptr) {
				outs << ",\n";
				OUT_PADDED(outs, level, "\"stack_ptr\" : ");
				outs << "\"" << std::hex << stack_ptr << "\"";
			}
			if (frames_count) {
				outs << ",\n";
				OUT_PADDED(outs, level, "\"frames_count\" : ");
				outs << std::dec << frames_count;
			}
			if (thread_state != THREAD_STATE_UNKNOWN) {
				outs << ",\n";
				OUT_PADDED(outs, level, "\"thread_state\" : ");
				outs << "\"" << translate_thread_state(thread_state) << "\"";

				if (thread_state == THREAD_STATE_WAITING) {
					outs << ",\n";
					OUT_PADDED(outs, level, "\"thread_wait_reason\" : ");
					outs << "\"" << translate_wait_reason(thread_wait_reason) << "\"";
				}
			}
			if (susp_addr) {
				outs << ",\n";
				if (this->module && this->moduleSize) {
					OUT_PADDED(outs, level, "\"susp_addr\" : ");
				}
				else {
					OUT_PADDED(outs, level, "\"susp_return_addr\" : ");
				}
				outs << "\"" << std::hex << susp_addr << "\"";
			}
			if (this->module) {
				outs << ",\n";
				OUT_PADDED(outs, level, "\"protection\" : ");
				outs << "\"" << std::hex << protection << "\"";
				if (stats.isFilled()) {
					outs << ",\n";
					stats.toJSON(outs, level);
				}
			}
		}

		const virtual bool toJSON(std::stringstream& outs, size_t level, const pesieve::t_json_level &jdetails)
		{
			OUT_PADDED(outs, level, "\"thread_scan\" : {\n");
			fieldsToJSON(outs, level + 1, jdetails);
			outs << "\n";
			OUT_PADDED(outs, level, "}");
			return true;
		}

		DWORD tid;
		ULONGLONG susp_addr;
		DWORD protection;
		ULONGLONG stack_ptr;
		size_t frames_count;
		DWORD thread_state;
		DWORD thread_wait_reason;
		DWORD thread_wait_time;
		std::set<ThSusIndicator> indicators;
		AreaEntropyStats stats;
		bool is_code;
	};

	//!  A custom structure keeping a fragment of a thread context
	typedef struct _ctx_details {
		bool is64b;
		ULONGLONG rip;
		ULONGLONG rsp;
		ULONGLONG rbp;
		ULONGLONG last_ret; // the last return address on the stack
		ULONGLONG ret_on_stack; // the last return address stored on the stack
		bool is_ret_as_syscall;
		bool is_ret_in_frame;
		bool is_managed; // does it contain .NET modules
		size_t stackFramesCount;
		std::set<ULONGLONG> shcCandidates;

		_ctx_details(bool _is64b = false, ULONGLONG _rip = 0, ULONGLONG _rsp = 0, ULONGLONG _rbp = 0, ULONGLONG _ret_addr = 0)
			: is64b(_is64b), rip(_rip), rsp(_rsp), rbp(_rbp), last_ret(_ret_addr), ret_on_stack(0), is_ret_as_syscall(false), is_ret_in_frame(false),
			stackFramesCount(0),
			is_managed(false)
		{
		}

		void init(bool _is64b = false, ULONGLONG _rip = 0, ULONGLONG _rsp = 0, ULONGLONG _rbp = 0, ULONGLONG _ret_addr = 0)
		{
			this->is64b = _is64b;
			this->rip = _rip;
			this->rsp = _rsp;
			this->rbp = _rbp;
			this->last_ret = _ret_addr;
		}

	} ctx_details;

	//!  A scanner for threads
	//!  Stack-scan inspired by the idea presented here: https://github.com/thefLink/Hunt-Sleeping-Beacons
	class ThreadScanner : public ProcessFeatureScanner {
	public:
		ThreadScanner(HANDLE hProc, bool _isReflection, bool _isManaged, const util::thread_info& _info, ModulesInfo& _modulesInfo, peconv::ExportsMapper* _exportsMap, ProcessSymbolsManager* _symbols)
			: ProcessFeatureScanner(hProc), isReflection(_isReflection), isManaged(_isManaged),
			info(_info), modulesInfo(_modulesInfo), exportsMap(_exportsMap), symbols(_symbols)
		{
		}

		virtual ThreadScanReport* scanRemote();

	protected:
		static std::string choosePreferredFunctionName(const std::string& dbgSymbol, const std::string& manualSymbol);

		bool scanRemoteThreadCtx(HANDLE hThread, ThreadScanReport* my_report);
		bool isAddrInNamedModule(ULONGLONG addr);
		void printThreadInfo(const util::thread_info& threadi);
		std::string resolveLowLevelFuncName(const ULONGLONG addr, size_t maxDisp=25);
		bool printResolvedAddr(ULONGLONG addr);
		bool fetchThreadCtxDetails(IN HANDLE hProcess, IN HANDLE hThread, OUT ctx_details& c);
		size_t fillCallStackInfo(IN HANDLE hProcess, IN HANDLE hThread, IN LPVOID ctx, IN OUT ctx_details& cDetails);
		size_t analyzeCallStack(IN const std::vector<ULONGLONG> &stack_frame, IN OUT ctx_details& cDetails);
		bool checkReturnAddrIntegrity(IN const std::vector<ULONGLONG>& callStack);
		bool fillAreaStats(ThreadScanReport* my_report);
		bool reportSuspiciousAddr(ThreadScanReport* my_report, ULONGLONG susp_addr);
		bool filterDotNet(ThreadScanReport& my_report);

		bool isReflection;
		bool isManaged;
		const util::thread_info& info;
		ModulesInfo& modulesInfo;
		peconv::ExportsMapper* exportsMap;
		ProcessSymbolsManager* symbols;
	};

}; //namespace pesieve
