// simplewall
// Copyright (c) 2016, 2017 Henry++

#include <winsock2.h>
#include <ws2ipdef.h>
#include <windns.h>
#include <mstcpip.h>
#include <windows.h>
#include <iphlpapi.h>
#include <subauth.h>
#include <fwpmu.h>
#include <dbt.h>

#include "main.h"
#include "rapp.h"
#include "routine.h"

#include "pugixml\pugixml.hpp"

#include "resource.h"

CONST UINT WM_FINDMSGSTRING = RegisterWindowMessage (FINDMSGSTRING);

rapp app (APP_NAME, APP_NAME_SHORT, APP_VERSION, APP_COPYRIGHT);

std::unordered_map<size_t, ITEM_APPLICATION> apps;
std::unordered_map<size_t, std::unordered_map<size_t, BOOL>> apps_rules;
std::unordered_map<size_t, __time64_t> notifications;

std::vector<ITEM_COLOR> colors;
std::vector<ITEM_PROCESS> processes;
std::vector<ITEM_PROTOCOL> protocols;
std::vector<ITEM_RULE> rules_system;
std::vector<ITEM_RULE> rules_custom;
std::vector<ITEM_RULE> rules_blocklist;

STATIC_DATA config;

DWORD Mps_ChangeConfig (BOOL is_stop)
{
	DWORD result = 0;
	SC_HANDLE scm = OpenSCManager (nullptr, nullptr, SC_MANAGER_ALL_ACCESS);

	if (!scm)
	{
		WDBG (L"OpenSCManager failed. Return value: 0x%.8lx.", GetLastError ());
	}
	else
	{
		LPCWSTR arr[] = {L"mpssvc", L"mpsdrv"};

		for (INT i = 0; i < _countof (arr); i++)
		{
			SC_HANDLE sc = OpenService (scm, arr[i], SERVICE_CHANGE_CONFIG | SERVICE_QUERY_STATUS | SERVICE_STOP);

			if (!sc)
			{
				WDBG (L"OpenService failed. Return value: 0x%.8lx.", GetLastError ());
			}
			else
			{
				if (is_stop)
				{
					DWORD dwBytesNeeded = 0;
					SERVICE_STATUS_PROCESS ssp = {0};

					if (!QueryServiceStatusEx (sc, SC_STATUS_PROCESS_INFO, (LPBYTE)&ssp, sizeof (ssp), &dwBytesNeeded))
					{
						WDBG (L"QueryServiceStatusEx failed. Return value: 0x%.8lx.", GetLastError ());
					}
					else
					{
						if (ssp.dwCurrentState != SERVICE_STOPPED)
						{
							if (!ControlService (sc, SERVICE_CONTROL_STOP, (LPSERVICE_STATUS)&ssp))
							{
								WDBG (L"ControlService failed. Return value: 0x%.8lx.", GetLastError ());
							}
							else
							{
								while (ssp.dwCurrentState == SERVICE_STOP_PENDING)
								{
									_r_sleep (50);

									if (!QueryServiceStatusEx (sc, SC_STATUS_PROCESS_INFO, (LPBYTE)&ssp, sizeof (ssp), &dwBytesNeeded))
									{
										WDBG (L"QueryServiceStatusEx failed. Return value: 0x%.8lx.", GetLastError ());
										break;
									}
								}
							}
						}
					}
				}

				if (!ChangeServiceConfig (sc, SERVICE_NO_CHANGE, is_stop ? SERVICE_DISABLED : SERVICE_AUTO_START, SERVICE_NO_CHANGE, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr))
				{
					WDBG (L"ChangeServiceConfig failed. Return value: 0x%.8lx.", GetLastError ());
				}

				CloseServiceHandle (sc);
			}
		}

		// start services
		if (!is_stop)
		{
			for (INT i = 0; i < _countof (arr); i++)
			{
				SC_HANDLE sc = OpenService (scm, arr[i], SERVICE_QUERY_STATUS | SERVICE_START);

				if (!sc)
				{
					WDBG (L"OpenService failed. Return value: 0x%.8lx.", GetLastError ());
				}
				else
				{
					DWORD dwBytesNeeded = 0;
					SERVICE_STATUS_PROCESS ssp = {0};

					if (!QueryServiceStatusEx (sc, SC_STATUS_PROCESS_INFO, (LPBYTE)&ssp, sizeof (ssp), &dwBytesNeeded))
					{
						WDBG (L"QueryServiceStatusEx failed. Return value: 0x%.8lx.", GetLastError ());
					}
					else
					{
						if (ssp.dwCurrentState != SERVICE_RUNNING)
						{
							if (!StartService (sc, 0, nullptr))
							{
								WDBG (L"StartService failed. Return value: 0x%.8lx.", GetLastError ());
							}
						}

						CloseServiceHandle (sc);
					}
				}
			}
		}

		CloseServiceHandle (scm);
	}

	return result;
}

VOID _app_refreshstatus (HWND hwnd, BOOL first_part, BOOL second_part)
{
	if (first_part)
		_r_status_settext (hwnd, IDC_STATUSBAR, 0, _r_fmt (I18N (&app, IDS_STATUS_TOTAL, 0), apps.size ()));

	if (second_part)
	{
		switch (app.ConfigGet (L"Mode", Whitelist).AsUint ())
		{
			case Whitelist:
			{
				_r_status_settext (hwnd, IDC_STATUSBAR, 1, I18N (&app, IDS_MODE_WHITELIST, 0));
				break;
			}

			case Blacklist:
			{
				_r_status_settext (hwnd, IDC_STATUSBAR, 1, I18N (&app, IDS_MODE_BLACKLIST, 0));
				break;
			}

			case TrustNoOne:
			{
				_r_status_settext (hwnd, IDC_STATUSBAR, 1, I18N (&app, IDS_MODE_TRUSTNOONE, 0));
				break;
			}
		}
	}
}

VOID _app_getinfo (LPCWSTR path, size_t* icon_id, LPWSTR info, size_t info_len)
{
	if (app.ConfigGet (L"IsIconsHidden", FALSE).AsBool ())
	{
		*icon_id = LAST_VALUE;
	}
	else
	{
		SHFILEINFO shfi = {0};
		SHGetFileInfo (path, 0, &shfi, sizeof (shfi), SHGFI_SYSICONINDEX);

		if (icon_id)
			*icon_id = shfi.iIcon;
	}

	HINSTANCE h = LoadLibraryEx (path, nullptr, DONT_RESOLVE_DLL_REFERENCES | LOAD_LIBRARY_AS_DATAFILE);

	if (h)
	{
		HRSRC hv = FindResource (h, MAKEINTRESOURCE (VS_VERSION_INFO), RT_VERSION);

		if (hv)
		{
			HGLOBAL hg = LoadResource (h, hv);

			if (hg)
			{
				LPVOID versionInfo = LockResource (hg);

				if (versionInfo)
				{
					UINT vLen = 0, langD = 0;
					LPVOID retbuf = nullptr;

					WCHAR author_entry[MAX_PATH] = {0};
					WCHAR description_entry[MAX_PATH] = {0};
					WCHAR version_entry[MAX_PATH] = {0};

					BOOL result = VerQueryValue (versionInfo, L"\\VarFileInfo\\Translation", &retbuf, &vLen);

					if (result && vLen == 4)
					{
						memcpy (&langD, retbuf, 4);
						StringCchPrintf (author_entry, _countof (author_entry), L"\\StringFileInfo\\%02X%02X%02X%02X\\CompanyName", (langD & 0xff00) >> 8, langD & 0xff, (langD & 0xff000000) >> 24, (langD & 0xff0000) >> 16);
						StringCchPrintf (description_entry, _countof (description_entry), L"\\StringFileInfo\\%02X%02X%02X%02X\\FileDescription", (langD & 0xff00) >> 8, langD & 0xff, (langD & 0xff000000) >> 24, (langD & 0xff0000) >> 16);
						StringCchPrintf (version_entry, _countof (version_entry), L"\\StringFileInfo\\%02X%02X%02X%02X\\FileVersion", (langD & 0xff00) >> 8, langD & 0xff, (langD & 0xff000000) >> 24, (langD & 0xff0000) >> 16);
					}
					else
					{
						StringCchPrintf (author_entry, _countof (author_entry), L"\\StringFileInfo\\%04X04B0\\CompanyName", GetUserDefaultLangID ());
						StringCchPrintf (description_entry, _countof (description_entry), L"\\StringFileInfo\\%04X04B0\\FileDescription", GetUserDefaultLangID ());
						StringCchPrintf (version_entry, _countof (version_entry), L"\\StringFileInfo\\%04X04B0\\FileVersion", GetUserDefaultLangID ());
					}

					if (info)
					{
						if (VerQueryValue (versionInfo, description_entry, &retbuf, &vLen))
						{
							StringCchCat (info, info_len, static_cast<LPCWSTR>(retbuf));

							UINT length = 0;
							VS_FIXEDFILEINFO* verInfo = nullptr;

							if (VerQueryValue (versionInfo, L"\\", reinterpret_cast<LPVOID*>(&verInfo), &length))
								StringCchCat (info, info_len, _r_fmt (L" %d.%d.%d.%d", HIWORD (verInfo->dwProductVersionMS), LOWORD (verInfo->dwProductVersionMS), HIWORD (verInfo->dwProductVersionLS), LOWORD (verInfo->dwProductVersionLS)));

							StringCchCat (info, info_len, L"\r\n");
						}

						if (VerQueryValue (versionInfo, author_entry, &retbuf, &vLen))
						{
							StringCchCat (info, info_len, static_cast<LPCWSTR>(retbuf));
							StringCchCat (info, info_len, L"\r\n");
						}
					}
				}
			}

			// free memory
			UnlockResource (hg);
			FreeResource (hg);
		}

		FreeLibrary (h); // free memory
	}
}

size_t _app_getposition (HWND hwnd, size_t hash)
{
	for (size_t i = 0; i < _r_listview_getitemcount (hwnd, IDC_LISTVIEW); i++)
	{
		if ((size_t)_r_listview_getlparam (hwnd, IDC_LISTVIEW, i) == hash)
			return i;
	}

	return LAST_VALUE;
}

size_t _app_addapplication (HWND hwnd, LPCWSTR path, BOOL is_silent, BOOL is_checked)
{
	if (!path)
		return 0;

	const size_t hash = _r_str_hash (path);

	_R_SPINLOCK (config.lock_access);

	if (apps.find (hash) == apps.end ())
	{
		ITEM_APPLICATION* ptr = &apps[hash]; // application pointer

		BOOL is_ntoskrnl = (hash == config.ntoskrnl_hash);

		StringCchCopy (ptr->display_path, _countof (ptr->display_path), path);
		StringCchCopy (ptr->real_path, _countof (ptr->real_path), is_ntoskrnl ? _r_path_expand (PATH_NTOSKRNL) : path);
		StringCchCopy (ptr->file_dir, _countof (ptr->file_dir), path);
		StringCchCopy (ptr->file_name, _countof (ptr->file_name), _r_path_extractfile (path));
		StringCchCopy (ptr->info, _countof (ptr->info), ptr->real_path);
		StringCchCat (ptr->info, _countof (ptr->info), L"\r\n");

		PathRemoveFileSpec (ptr->file_dir);

		const DWORD dwAttr = GetFileAttributes (ptr->real_path);

		ptr->is_success = TRUE;
		ptr->is_checked = is_checked;
		ptr->is_silent = is_silent;
		ptr->is_system = is_ntoskrnl || (((dwAttr != INVALID_FILE_ATTRIBUTES && dwAttr & FILE_ATTRIBUTE_SYSTEM) != 0)) || (_wcsnicmp (ptr->real_path, config.windows_dir, config.wd_length) == 0);
		ptr->is_network = PathIsNetworkPath (ptr->file_dir);

		if (!ptr->is_network)
			_app_getinfo (ptr->real_path, &ptr->icon_id, ptr->info, _countof (ptr->info)); // read file information

		StringCchCopy (ptr->info, _countof (ptr->info), rstring (ptr->info).Trim (L"\r\n "));

		const size_t item = _r_listview_getitemcount (hwnd, IDC_LISTVIEW);

		config.is_firstapply = FALSE; // lock checkbox notifications

		_r_listview_additem (hwnd, IDC_LISTVIEW, app.ConfigGet (L"ShowFilenames", TRUE).AsBool () ? ptr->file_name : path, item, 0, ptr->icon_id, LAST_VALUE, hash);
		_r_listview_setcheckstate (hwnd, IDC_LISTVIEW, item, is_checked);

		config.is_firstapply = TRUE; // unlock checkbox notifications
	}

	_R_SPINUNLOCK (config.lock_access);

	return hash;
}

VOID _wfp_destroyfilters (BOOL is_forced)
{
	_R_SPINLOCK (config.lock_access);

	for (auto& p : apps)
		p.second.is_success = TRUE;

	_R_SPINUNLOCK (config.lock_access);

	DWORD result = FwpmTransactionBegin (config.hengine, 0);

	if (result != ERROR_SUCCESS)
	{
		WDBG (L"FwpmTransactionBegin failed. Return value: 0x%.8lx.", result);
	}
	else
	{
		HANDLE henum = nullptr;
		result = FwpmFilterCreateEnumHandle (config.hengine, nullptr, &henum);

		if (result != ERROR_SUCCESS)
		{
			WDBG (L"FwpmFilterCreateEnumHandle failed. Return value: 0x%.8lx.", result);
		}
		else
		{
			UINT32 count = 0;
			FWPM_FILTER** matchingFwpFilter = nullptr;

			result = FwpmFilterEnum (config.hengine, henum, 0xFFFFFFFF, &matchingFwpFilter, &count);

			if (result != ERROR_SUCCESS)
			{
				WDBG (L"FwpmFilterEnum failed. Return value: 0x%.8lx.", result);
			}
			else
			{
				if (matchingFwpFilter)
				{
					for (UINT32 i = 0; i < count; i++)
					{
						if (matchingFwpFilter[i]->providerKey && memcmp (matchingFwpFilter[i]->providerKey, &GUID_WfpProvider, sizeof (GUID)) == 0)
							FwpmFilterDeleteById (config.hengine, matchingFwpFilter[i]->filterId);
					}

					FwpmFreeMemory ((LPVOID*)&matchingFwpFilter);
				}
			}
		}

		if (henum)
			FwpmFilterDestroyEnumHandle (config.hengine, henum);

		// destroy callouts
		FwpmCalloutDeleteByKey (config.hengine, &GUID_WfpOutboundCallout4);
		FwpmCalloutDeleteByKey (config.hengine, &GUID_WfpOutboundCallout6);

		FwpmCalloutDeleteByKey (config.hengine, &GUID_WfpInboundCallout4);
		FwpmCalloutDeleteByKey (config.hengine, &GUID_WfpInboundCallout6);

		FwpmCalloutDeleteByKey (config.hengine, &GUID_WfpListenCallout4);
		FwpmCalloutDeleteByKey (config.hengine, &GUID_WfpListenCallout6);

		// destroy sublayer
		FwpmSubLayerDeleteByKey (config.hengine, &GUID_WfpSublayer);

		// destroy provider
		result = FwpmProviderDeleteByKey (config.hengine, &GUID_WfpProvider);

		if (result != ERROR_SUCCESS && result != FWP_E_PROVIDER_NOT_FOUND)
			WDBG (L"FwpmProviderDeleteByKey failed. Return value: 0x%.8lx.", result);

		FwpmTransactionCommit (config.hengine);
	}

	if (is_forced)
	{
		// set icons
		app.SetIcon (IDI_INACTIVE);
		app.TraySetInfo (_r_loadicon (app.GetHINSTANCE (), MAKEINTRESOURCE (IDI_INACTIVE), GetSystemMetrics (SM_CXSMICON)), APP_NAME);

		SetDlgItemText (app.GetHWND (), IDC_START_BTN, I18N (&app, IDS_TRAY_START, 0));

		config.is_filtersinstalled = FALSE;
	}
}

VOID _wfp_createcallout (HANDLE h, const GUID layer_key, const GUID callout_key)
{
	FWPM_CALLOUT0 callout = {0};
	UINT32 callout_id = 0;

	callout.displayData.name = APP_NAME;
	callout.displayData.description = APP_NAME;

	callout.flags = FWPM_CALLOUT_FLAG_PERSISTENT;

	callout.providerKey = (LPGUID)&GUID_WfpProvider;
	callout.calloutKey = callout_key;
	callout.applicableLayer = layer_key;

	DWORD result = FwpmCalloutAdd (h, &callout, nullptr, &callout_id);

	if (result != ERROR_SUCCESS && result != FWP_E_ALREADY_EXISTS)
		WDBG (L"FwpmCalloutAdd failed. Return value: 0x%.8lx.", result);
}

VOID _wfp_createfilter (LPCWSTR name, FWPM_FILTER_CONDITION* lpcond, UINT32 const count, UINT8 weight, GUID layer, GUID callout, FWP_ACTION_TYPE action, UINT32 flags = 0)
{
	FWPM_FILTER filter = {0};

	filter.flags = flags ? flags : FWPM_FILTER_FLAG_PERSISTENT;

	WCHAR fltr_name[64] = {0};
	WCHAR fltr_desc[128] = {0};

	if (!name)
		StringCchCopy (fltr_name, _countof (fltr_name), action == FWP_ACTION_BLOCK ? L"Block" : L"Permit");
	else
		StringCchCopy (fltr_name, _countof (fltr_name), name);

	StringCchPrintf (fltr_desc, _countof (fltr_desc), APP_NAME_SHORT L" - %s", fltr_name);

	filter.displayData.name = APP_NAME;
	filter.displayData.description = fltr_desc;

	filter.providerKey = (LPGUID)&GUID_WfpProvider;
	filter.layerKey = layer;
	filter.subLayerKey = GUID_WfpSublayer;

	filter.numFilterConditions = count;
	filter.filterCondition = lpcond;
	filter.action.type = action;
	filter.action.calloutKey = callout;

	filter.weight.type = FWP_UINT8;
	filter.weight.uint8 = weight;

	UINT64 filter_id = 0;
	DWORD result = FwpmFilterAdd (config.hengine, &filter, nullptr, &filter_id);

	if (result != ERROR_SUCCESS)
		WDBG (L"FwpmFilterAdd failed. Return value: 0x%.8lx (%s).", result, name);
}

INT CALLBACK _app_listviewcompare (LPARAM lp1, LPARAM lp2, LPARAM sortParam)
{
	BOOL isAsc = HIWORD (sortParam);
	BOOL isByFN = LOWORD (sortParam);

	size_t item1 = static_cast<size_t>(lp1);
	size_t item2 = static_cast<size_t>(lp2);

	INT result = 0;

	if (apps.find (item1) == apps.end () || apps.find (item2) == apps.end ())
		return 0;

	const ITEM_APPLICATION* app1 = &apps[item1];
	const ITEM_APPLICATION* app2 = &apps[item2];

	if (app1->is_checked && !app2->is_checked)
	{
		result = -1;
	}
	else if (!app1->is_checked && app2->is_checked)
	{
		result = 1;
	}
	else
	{
		result = _wcsicmp (isByFN ? app1->file_name : app1->file_dir, isByFN ? app2->file_name : app2->file_dir);
	}

	return isAsc ? -result : result;
}

VOID _app_listviewsort (HWND hwnd)
{
	LPARAM lparam = MAKELPARAM (app.ConfigGet (L"SortMode", 1).AsUint (), app.ConfigGet (L"IsSortDescending", FALSE).AsBool ());

	CheckMenuRadioItem (GetMenu (hwnd), IDM_SORTBYFNAME, IDM_SORTBYFDIR, (LOWORD (lparam) ? IDM_SORTBYFNAME : IDM_SORTBYFDIR), MF_BYCOMMAND);
	CheckMenuItem (GetMenu (hwnd), IDM_SORTISDESCEND, MF_BYCOMMAND | (HIWORD (lparam) ? MF_CHECKED : MF_UNCHECKED));

	SendDlgItemMessage (hwnd, IDC_LISTVIEW, LVM_SORTITEMS, lparam, (LPARAM)&_app_listviewcompare);
}

BOOL _app_parseaddress (LPCWSTR address, ITEM_ADDRESS* ptr)
{
	NET_ADDRESS_INFO ni;
	SecureZeroMemory (&ni, sizeof (ni));

	BYTE prefix = 0;
	DWORD result = ParseNetworkString (address, NET_STRING_IP_ADDRESS | NET_STRING_IP_NETWORK | NET_STRING_IP_ADDRESS_NO_SCOPE | NET_STRING_IP_SERVICE, &ni, &ptr->port, &prefix);

	if (result != ERROR_SUCCESS)
	{
		WDBG (L"ParseNetworkString failed. Return value: 0x%.8lx. (%s)", result, address);
	}
	else
	{
		if (ptr)
		{
			ptr->af = ni.IpAddress.sa_family;

			if (ni.IpAddress.sa_family == AF_INET)
			{
				ConvertLengthToIpv4Mask (prefix, (PULONG)&ptr->v4mask);

				ptr->v4mask = ntohl (ptr->v4mask);
				ptr->v4address = ntohl (ni.Ipv4Address.sin_addr.S_un.S_addr);

				if (ptr->ptr4)
				{
					ptr->ptr4->mask = ptr->v4mask;
					ptr->ptr4->addr = ptr->v4address;
				}
			}
			else if (ni.IpAddress.sa_family == AF_INET6)
			{
				ptr->v6prefix = prefix;
				memcpy (ptr->v6address, ni.Ipv6Address.sin6_addr.u.Byte, FWP_V6_ADDR_SIZE);

				if (ptr->ptr6)
				{
					ptr->ptr6->prefixLength = prefix;
					memcpy (ptr->ptr6->addr, ptr->v6address, FWP_V6_ADDR_SIZE);
				}
			}
		}

		return TRUE;
	}

	return FALSE;
}

bool IsPort (LPCWSTR rule)
{
	if (!rule)
		return false;

	const size_t length = wcslen (rule);

	for (size_t i = 0; i < length; i++)
	{
		if (iswdigit (rule[i]) == 0 && rule[i] != L'-')
			return false;
	}

	return true;
}

BOOL _wfp_createrulefilter (LPCWSTR name, LPCWSTR rule, LPCWSTR path, EnumDirection dir, UINT8 protocol, ADDRESS_FAMILY af, FWP_ACTION_TYPE action, UINT8 weight)
{
	BOOL is_success = FALSE;

	UINT32 count = 0;
	FWPM_FILTER_CONDITION fwfc[6] = {0};

	FWP_BYTE_BLOB* blob = nullptr;

	const BOOL is_port = IsPort (rule);

	FWP_V4_ADDR_AND_MASK addrmask4 = {0};
	FWP_V6_ADDR_AND_MASK addrmask6 = {0};

	ITEM_ADDRESS addr;
	SecureZeroMemory (&addr, sizeof (addr));

	FWP_RANGE range;
	SecureZeroMemory (&range, sizeof (range));

	UINT32 addr_index = UINT32 (-1);
	UINT32 port_index = UINT32 (-1);

	if (path)
	{
		DWORD result = FwpmGetAppIdFromFileName (path, &blob);

		if (result != ERROR_SUCCESS)
		{
			WDBG (L"FwpmGetAppIdFromFileName failed. Return value: 0x%.8lx. (%s)", result, path);
		}
		else
		{
			fwfc[count].fieldKey = FWPM_CONDITION_ALE_APP_ID;
			fwfc[count].matchType = FWP_MATCH_EQUAL;
			fwfc[count].conditionValue.type = FWP_BYTE_BLOB_TYPE;
			fwfc[count].conditionValue.byteBlob = blob;

			count += 1;

			is_success = TRUE;
		}
	}

	if (protocol)
	{
		fwfc[count].fieldKey = FWPM_CONDITION_IP_PROTOCOL;
		fwfc[count].matchType = FWP_MATCH_EQUAL;
		fwfc[count].conditionValue.type = FWP_UINT8;
		fwfc[count].conditionValue.uint8 = protocol;

		count += 1;
	}

	if (rule)
	{
		const rstring rule_s = rule;
		const size_t range_pos = rule_s.Find (L'-');

		WCHAR range_start[128] = {0};
		WCHAR range_end[128] = {0};

		if (range_pos != rstring::npos)
		{
			StringCchCopy (range_start, _countof (range_start), rule_s.Midded (0, range_pos));
			StringCchCopy (range_end, _countof (range_end), rule_s.Midded (range_pos + 1));
		}

		if (is_port)
		{
			// ...port
			if (range_pos != rstring::npos)
			{
				range.valueLow.type = FWP_UINT16;
				range.valueLow.uint16 = (UINT16)wcstoul (range_start, nullptr, 10);

				range.valueHigh.type = FWP_UINT16;
				range.valueHigh.uint16 = (UINT16)wcstoul (range_end, nullptr, 10);
			}
			else
			{
				//pcond[count].fieldKey = FWPM_CONDITION_IP_LOCAL_PORT;
				fwfc[count].matchType = FWP_MATCH_EQUAL;
				fwfc[count].conditionValue.type = FWP_UINT16;
				fwfc[count].conditionValue.uint16 = (UINT16)wcstoul (rule, nullptr, 10);

				port_index = count;
				count += 1;
			}
		}
		else
		{
			// ...address
			if (range_pos != rstring::npos)
			{
				// parse range start
				if (_app_parseaddress (range_start, &addr))
				{
					af = addr.af;

					if (af == AF_INET)
					{
						range.valueLow.type = FWP_UINT32;
						range.valueLow.uint32 = addr.v4address;
					}
					else if (af == AF_INET6)
					{
						range.valueLow.type = FWP_BYTE_ARRAY16_TYPE;
						memcpy (range.valueLow.byteArray16->byteArray16, addr.v6address, FWP_V6_ADDR_SIZE);
					}
				}

				// parse range end
				if (_app_parseaddress (range_end, &addr))
				{
					af = addr.af;

					if (af == AF_INET)
					{
						range.valueHigh.type = FWP_UINT32;
						range.valueHigh.uint32 = addr.v4address;
					}
					else if (af == AF_INET6)
					{
						range.valueHigh.type = FWP_BYTE_ARRAY16_TYPE;
						memcpy (range.valueHigh.byteArray16->byteArray16, addr.v6address, FWP_V6_ADDR_SIZE);
					}
				}
			}
			else
			{
				addr.ptr4 = &addrmask4;
				addr.ptr6 = &addrmask6;

				if (_app_parseaddress (rule, &addr))
				{
					af = addr.af;

					if (af == AF_INET)
					{
						fwfc[count].matchType = FWP_MATCH_EQUAL;
						fwfc[count].conditionValue.type = FWP_V4_ADDR_MASK;
						fwfc[count].conditionValue.v4AddrMask = &addrmask4;

						addr_index = count;
						count += 1;
					}
					else if (af == AF_INET6)
					{
						fwfc[count].matchType = FWP_MATCH_EQUAL;
						fwfc[count].conditionValue.type = FWP_V6_ADDR_MASK;
						fwfc[count].conditionValue.v6AddrMask = &addrmask6;

						addr_index = count;
						count += 1;
					}

					// set port if available
					if (addr.port)
					{
						fwfc[count].matchType = FWP_MATCH_EQUAL;
						fwfc[count].conditionValue.type = FWP_UINT16;
						fwfc[count].conditionValue.uint16 = addr.port;

						port_index = count;
						count += 1;
					}
				}
			}
		}

		if (range_pos != rstring::npos)
		{
			//pcond[count].fieldKey = FWPM_CONDITION_IP_LOCAL_PORT;
			fwfc[count].matchType = FWP_MATCH_RANGE;
			fwfc[count].conditionValue.type = FWP_RANGE_TYPE;
			fwfc[count].conditionValue.rangeValue = &range;

			if (is_port)
				port_index = count;
			else
				addr_index = count;

			count += 1;
		}
	}

	// create filters
	if (dir == Out || dir == Both)
	{
		if (addr_index != UINT32 (-1))
			fwfc[addr_index].fieldKey = FWPM_CONDITION_IP_REMOTE_ADDRESS;

		if (port_index != UINT32 (-1))
			fwfc[port_index].fieldKey = FWPM_CONDITION_IP_REMOTE_PORT;

		if (af == AF_INET || af == AF_UNSPEC)
			_wfp_createfilter (name, fwfc, count, weight, FWPM_LAYER_ALE_AUTH_CONNECT_V4, GUID_WfpOutboundCallout4, action);

		if (af == AF_INET6 || af == AF_UNSPEC)
			_wfp_createfilter (name, fwfc, count, weight, FWPM_LAYER_ALE_AUTH_CONNECT_V6, GUID_WfpOutboundCallout6, action);
	}

	if (dir == In || dir == Both)
	{
		if (addr_index != UINT32 (-1))
			fwfc[addr_index].fieldKey = FWPM_CONDITION_IP_LOCAL_ADDRESS;

		if (port_index != UINT32 (-1))
			fwfc[port_index].fieldKey = FWPM_CONDITION_IP_LOCAL_PORT;

		if (af == AF_INET || af == AF_UNSPEC)
			_wfp_createfilter (name, fwfc, count, weight, FWPM_LAYER_ALE_AUTH_RECV_ACCEPT_V4, GUID_WfpInboundCallout4, action);

		if (af == AF_INET6 || af == AF_UNSPEC)
			_wfp_createfilter (name, fwfc, count, weight, FWPM_LAYER_ALE_AUTH_RECV_ACCEPT_V6, GUID_WfpInboundCallout6, action);

		// only if address is not set
		if (addr_index == UINT32 (-1) && !protocol)
		{
			if (dir == In || dir == Both)
			{
				if (af == AF_INET || af == AF_UNSPEC)
					_wfp_createfilter (name, fwfc, count, weight, FWPM_LAYER_ALE_AUTH_LISTEN_V4, GUID_WfpListenCallout4, action);

				if (af == AF_INET6 || af == AF_UNSPEC)
					_wfp_createfilter (name, fwfc, count, weight, FWPM_LAYER_ALE_AUTH_LISTEN_V6, GUID_WfpListenCallout6, action);
			}
		}
	}

	if (blob)
		FwpmFreeMemory ((LPVOID*)&blob);

	return is_success;
}

VOID _app_loadrules (HWND hwnd, LPCWSTR path, LPCWSTR section, std::vector<ITEM_RULE>* ptr)
{
	if (!ptr)
		return;

	if (!section)
		apps_rules.clear ();

	ptr->clear ();

	pugi::xml_document doc;

	if (doc.load_file (path, pugi::parse_default, pugi::encoding_auto))
	{
		pugi::xml_node root = doc.child (L"root");

		if (root)
		{
			for (pugi::xml_node item = root.child (L"item"); item; item = item.next_sibling (L"item"))
			{
				ITEM_RULE rule;
				SecureZeroMemory (&rule, sizeof (rule));

				StringCchCopy (rule.name, _countof (rule.name), item.attribute (L"name").as_string ());
				StringCchCopy (rule.rule, _countof (rule.rule), item.attribute (L"rule").as_string ());

				rule.dir = (EnumDirection)item.attribute (L"dir").as_uint ();
				rule.protocol = (UINT8)item.attribute (L"protocol").as_uint ();
				rule.version = (ADDRESS_FAMILY)item.attribute (L"version").as_uint ();

				rule.is_block = item.attribute (L"is_block").as_bool ();
				rule.is_enabled = item.attribute (L"is_enabled").as_bool ();

				if (section)
					rule.is_enabled = app.ConfigGet (rule.name, rule.is_enabled, section).AsBool ();

				if (!section)
				{
					rstring::rvector vc = rstring (item.attribute (L"apps").as_string ()).AsVector (RULE_DELIMETER);

					for (size_t i = 0; i < vc.size (); i++)
					{
						const size_t hash = vc.at (i).Hash ();

						apps_rules[hash][ptr->size ()] = TRUE;

						if (apps.find (hash) == apps.end ())
							_app_addapplication (hwnd, path, FALSE, FALSE);
					}
				}

				ptr->push_back (rule);
			}
		}
	}
}

VOID _app_profilesave (HWND hwnd)
{
	_R_SPINLOCK (config.lock_access);

	// apps rules
	{
		pugi::xml_document doc;
		pugi::xml_node node = doc.append_child (L"root");

		if (node)
		{
			for (size_t i = 0; i < _r_listview_getitemcount (hwnd, IDC_LISTVIEW); i++)
			{
				const size_t hash = (size_t)_r_listview_getlparam (hwnd, IDC_LISTVIEW, i);

				if (apps.find (hash) == apps.end ())
					continue;

				ITEM_APPLICATION const* ptr = &apps[hash];

				pugi::xml_node item = node.append_child (L"item");

				if (item)
				{
					item.append_attribute (L"path").set_value (ptr->display_path);
					item.append_attribute (L"is_silent").set_value (ptr->is_silent);
					item.append_attribute (L"is_enabled").set_value (ptr->is_checked);
				}
			}

			doc.save_file (config.apps_path, L"\t", pugi::format_indent | pugi::format_write_bom, pugi::encoding_utf16);
		}
	}

	// custom rules
	{
		pugi::xml_document doc;
		pugi::xml_node node = doc.append_child (L"root");

		if (node)
		{
			for (size_t i = 0; i < rules_custom.size (); i++)
			{
				ITEM_RULE const* ptr = &rules_custom.at (i);

				pugi::xml_node item = node.append_child (L"item");

				if (item)
				{
					rstring arr;

					for (auto const &p : apps_rules)
					{
						if (apps.find (p.first) != apps.end () && p.second.find (i) != p.second.end ())
						{
							arr.Append (apps[p.first].display_path);
							arr.Append (L";");
						}
					}

					arr.Trim (L";");

					item.append_attribute (L"name").set_value (ptr->name);
					item.append_attribute (L"rule").set_value (ptr->rule);
					item.append_attribute (L"dir").set_value (ptr->dir);
					item.append_attribute (L"protocol").set_value (ptr->protocol);
					item.append_attribute (L"version").set_value (ptr->version);
					item.append_attribute (L"apps").set_value (arr);
					item.append_attribute (L"is_block").set_value (ptr->is_block);
					item.append_attribute (L"is_enabled").set_value (ptr->is_enabled);
				}
			}

			doc.save_file (config.rules_custom_path, L"\t", pugi::format_indent | pugi::format_write_bom, pugi::encoding_utf16);
		}
	}

	_R_SPINUNLOCK (config.lock_access);
}

VOID _wfp_installfilters ()
{
	_wfp_destroyfilters (FALSE); // destroy prevoius filters before

	DWORD result = FwpmTransactionBegin (config.hengine, 0);

	if (result != ERROR_SUCCESS)
	{
		WDBG (L"FwpmTransactionBegin failed. Return value: 0x%.8lx.", result);
	}
	else
	{
		// create provider
		FWPM_PROVIDER provider = {0};

		provider.displayData.name = APP_NAME;
		provider.displayData.description = APP_NAME;

		provider.providerKey = GUID_WfpProvider;
		provider.flags = FWPM_PROVIDER_FLAG_PERSISTENT;

		result = FwpmProviderAdd (config.hengine, &provider, nullptr);

		if (result != ERROR_SUCCESS && result != FWP_E_ALREADY_EXISTS)
		{
			WDBG (L"FwpmProviderAdd failed. Return value: 0x%.8lx.", result);
			FwpmTransactionAbort (config.hengine);
		}
		else
		{
			FWPM_SUBLAYER sublayer = {0};

			sublayer.displayData.name = APP_NAME;
			sublayer.displayData.description = APP_NAME;

			sublayer.providerKey = (LPGUID)&GUID_WfpProvider;
			sublayer.subLayerKey = GUID_WfpSublayer;
			sublayer.flags = FWPM_SUBLAYER_FLAG_PERSISTENT;
			sublayer.weight = (UINT16)app.ConfigGet (L"SublayerWeight", 0x0000ffff).AsUint ();

			result = FwpmSubLayerAdd (config.hengine, &sublayer, nullptr);

			if (result != ERROR_SUCCESS && result != FWP_E_ALREADY_EXISTS)
			{
				WDBG (L"FwpmSubLayerAdd failed. Return value: 0x%.8lx.", result);
				FwpmTransactionAbort (config.hengine);
			}
			else
			{
				_wfp_createcallout (config.hengine, FWPM_LAYER_ALE_AUTH_CONNECT_V4, GUID_WfpOutboundCallout4);
				_wfp_createcallout (config.hengine, FWPM_LAYER_ALE_AUTH_CONNECT_V6, GUID_WfpOutboundCallout6);

				_wfp_createcallout (config.hengine, FWPM_LAYER_ALE_AUTH_RECV_ACCEPT_V4, GUID_WfpInboundCallout4);
				_wfp_createcallout (config.hengine, FWPM_LAYER_ALE_AUTH_RECV_ACCEPT_V6, GUID_WfpInboundCallout6);

				_wfp_createcallout (config.hengine, FWPM_LAYER_ALE_AUTH_LISTEN_V4, GUID_WfpListenCallout4);
				_wfp_createcallout (config.hengine, FWPM_LAYER_ALE_AUTH_LISTEN_V6, GUID_WfpListenCallout6);

				const EnumMode mode = (EnumMode)app.ConfigGet (L"Mode", Whitelist).AsUint ();

				FWPM_FILTER_CONDITION fwfc[6] = {0};

				// add loopback connections permission
				{
					FWP_V4_ADDR_AND_MASK addrmask4 = {0};
					FWP_V6_ADDR_AND_MASK addrmask6 = {0};

					// First condition. Match only unicast addresses.
					fwfc[0].fieldKey = FWPM_CONDITION_IP_LOCAL_ADDRESS_TYPE;
					fwfc[0].matchType = FWP_MATCH_EQUAL;
					fwfc[0].conditionValue.type = FWP_UINT8;
					fwfc[0].conditionValue.uint8 = NlatUnicast;

					// Second condition. Match all loopback (localhost) data.
					fwfc[1].fieldKey = FWPM_CONDITION_FLAGS;
					fwfc[1].matchType = FWP_MATCH_EQUAL;
					fwfc[1].conditionValue.type = FWP_UINT32;
					fwfc[1].conditionValue.uint32 = FWP_CONDITION_FLAG_IS_LOOPBACK;

					_wfp_createfilter (nullptr, fwfc, 2, FILTER_WEIGHT_HIGHEST, FWPM_LAYER_ALE_AUTH_CONNECT_V4, GUID_WfpOutboundCallout4, FWP_ACTION_PERMIT);
					_wfp_createfilter (nullptr, fwfc, 2, FILTER_WEIGHT_HIGHEST, FWPM_LAYER_ALE_AUTH_CONNECT_V6, GUID_WfpOutboundCallout6, FWP_ACTION_PERMIT);

					_wfp_createfilter (nullptr, fwfc, 2, FILTER_WEIGHT_HIGHEST, FWPM_LAYER_ALE_AUTH_RECV_ACCEPT_V4, GUID_WfpInboundCallout4, FWP_ACTION_PERMIT);
					_wfp_createfilter (nullptr, fwfc, 2, FILTER_WEIGHT_HIGHEST, FWPM_LAYER_ALE_AUTH_RECV_ACCEPT_V6, GUID_WfpInboundCallout6, FWP_ACTION_PERMIT);

					_wfp_createfilter (nullptr, fwfc, 2, FILTER_WEIGHT_HIGHEST, FWPM_LAYER_ALE_AUTH_LISTEN_V4, GUID_WfpListenCallout4, FWP_ACTION_PERMIT);
					_wfp_createfilter (nullptr, fwfc, 2, FILTER_WEIGHT_HIGHEST, FWPM_LAYER_ALE_AUTH_LISTEN_V6, GUID_WfpListenCallout6, FWP_ACTION_PERMIT);

					// boot-time filters loopback permission
					if (app.ConfigGet (L"InstallBoottimeFilters", FALSE).AsBool ())
					{
						_wfp_createfilter (nullptr, fwfc, 2, FILTER_WEIGHT_HIGHEST, FWPM_LAYER_ALE_AUTH_CONNECT_V4, GUID_WfpOutboundCallout4, FWP_ACTION_PERMIT, FWPM_FILTER_FLAG_BOOTTIME);
						_wfp_createfilter (nullptr, fwfc, 2, FILTER_WEIGHT_HIGHEST, FWPM_LAYER_ALE_AUTH_CONNECT_V6, GUID_WfpOutboundCallout6, FWP_ACTION_PERMIT, FWPM_FILTER_FLAG_BOOTTIME);

						_wfp_createfilter (nullptr, fwfc, 2, FILTER_WEIGHT_HIGHEST, FWPM_LAYER_ALE_AUTH_RECV_ACCEPT_V4, GUID_WfpInboundCallout4, FWP_ACTION_PERMIT, FWPM_FILTER_FLAG_BOOTTIME);
						_wfp_createfilter (nullptr, fwfc, 2, FILTER_WEIGHT_HIGHEST, FWPM_LAYER_ALE_AUTH_RECV_ACCEPT_V6, GUID_WfpInboundCallout6, FWP_ACTION_PERMIT, FWPM_FILTER_FLAG_BOOTTIME);

						_wfp_createfilter (nullptr, fwfc, 2, FILTER_WEIGHT_HIGHEST, FWPM_LAYER_ALE_AUTH_LISTEN_V4, GUID_WfpListenCallout4, FWP_ACTION_PERMIT, FWPM_FILTER_FLAG_BOOTTIME);
						_wfp_createfilter (nullptr, fwfc, 2, FILTER_WEIGHT_HIGHEST, FWPM_LAYER_ALE_AUTH_LISTEN_V6, GUID_WfpListenCallout6, FWP_ACTION_PERMIT, FWPM_FILTER_FLAG_BOOTTIME);
					}

					// ipv4/ipv6 loopback
					LPCWSTR ip_list[] = {L"10.0.0.0/8", L"172.16.0.0/12", L"169.254.0.0/16", L"192.168.0.0/16", L"224.0.0.0/24", L"fd00::/8", L"fe80::/10"};

					for (size_t i = 0; i < _countof (ip_list); i++)
					{
						ITEM_ADDRESS addr;
						SecureZeroMemory (&addr, sizeof (addr));

						SecureZeroMemory (&addrmask4, sizeof (addrmask4));
						SecureZeroMemory (&addrmask6, sizeof (addrmask6));

						addr.ptr4 = &addrmask4;
						addr.ptr6 = &addrmask6;

						if (_app_parseaddress (ip_list[i], &addr))
						{
							//fwfc[2].fieldKey = FWPM_CONDITION_IP_LOCAL_ADDRESS;
							fwfc[2].matchType = FWP_MATCH_EQUAL;

							if (addr.af == AF_INET)
							{
								fwfc[2].conditionValue.type = FWP_V4_ADDR_MASK;
								fwfc[2].conditionValue.v4AddrMask = &addrmask4;

								fwfc[2].fieldKey = FWPM_CONDITION_IP_REMOTE_ADDRESS;
								_wfp_createfilter (nullptr, fwfc, 3, FILTER_WEIGHT_HIGHEST, FWPM_LAYER_ALE_AUTH_CONNECT_V4, GUID_WfpOutboundCallout4, FWP_ACTION_PERMIT);

								if (app.ConfigGet (L"InstallBoottimeFilters", FALSE).AsBool ())
									_wfp_createfilter (nullptr, fwfc, 3, FILTER_WEIGHT_HIGHEST, FWPM_LAYER_ALE_AUTH_CONNECT_V4, GUID_WfpOutboundCallout4, FWP_ACTION_PERMIT, FWPM_FILTER_FLAG_BOOTTIME);

								fwfc[2].fieldKey = FWPM_CONDITION_IP_LOCAL_ADDRESS;
								_wfp_createfilter (nullptr, fwfc, 3, FILTER_WEIGHT_HIGHEST, FWPM_LAYER_ALE_AUTH_RECV_ACCEPT_V4, GUID_WfpInboundCallout4, FWP_ACTION_PERMIT);

								if (app.ConfigGet (L"InstallBoottimeFilters", FALSE).AsBool ())
									_wfp_createfilter (nullptr, fwfc, 3, FILTER_WEIGHT_HIGHEST, FWPM_LAYER_ALE_AUTH_RECV_ACCEPT_V4, GUID_WfpInboundCallout4, FWP_ACTION_PERMIT, FWPM_FILTER_FLAG_BOOTTIME);
							}
							else if (addr.af == AF_INET6)
							{
								fwfc[2].conditionValue.type = FWP_V6_ADDR_MASK;
								fwfc[2].conditionValue.v6AddrMask = &addrmask6;

								fwfc[2].fieldKey = FWPM_CONDITION_IP_REMOTE_ADDRESS;
								_wfp_createfilter (nullptr, fwfc, 3, FILTER_WEIGHT_HIGHEST, FWPM_LAYER_ALE_AUTH_CONNECT_V6, GUID_WfpOutboundCallout6, FWP_ACTION_PERMIT);

								if (app.ConfigGet (L"InstallBoottimeFilters", FALSE).AsBool ())
									_wfp_createfilter (nullptr, fwfc, 3, FILTER_WEIGHT_HIGHEST, FWPM_LAYER_ALE_AUTH_CONNECT_V6, GUID_WfpOutboundCallout6, FWP_ACTION_PERMIT, FWPM_FILTER_FLAG_BOOTTIME);

								fwfc[2].fieldKey = FWPM_CONDITION_IP_LOCAL_ADDRESS;
								_wfp_createfilter (nullptr, fwfc, 3, FILTER_WEIGHT_HIGHEST, FWPM_LAYER_ALE_AUTH_RECV_ACCEPT_V6, GUID_WfpInboundCallout6, FWP_ACTION_PERMIT);

								if (app.ConfigGet (L"InstallBoottimeFilters", FALSE).AsBool ())
									_wfp_createfilter (nullptr, fwfc, 3, FILTER_WEIGHT_HIGHEST, FWPM_LAYER_ALE_AUTH_RECV_ACCEPT_V6, GUID_WfpInboundCallout6, FWP_ACTION_PERMIT, FWPM_FILTER_FLAG_BOOTTIME);
							}
						}
					}
				}

				if (mode != TrustNoOne)
				{
					fwfc[0].fieldKey = FWPM_CONDITION_IP_PROTOCOL;
					fwfc[0].matchType = FWP_MATCH_EQUAL;
					fwfc[0].conditionValue.type = FWP_UINT8;
					//fwfc[0].conditionValue.uint8 = IPPROTO_UDP;

					if (app.ConfigGet (L"AllowOutboundIcmp", TRUE).AsBool ())
					{
						fwfc[0].conditionValue.uint8 = IPPROTO_ICMP;
						_wfp_createfilter (nullptr, fwfc, 1, FILTER_WEIGHT_HIGHEST, FWPM_LAYER_ALE_AUTH_CONNECT_V4, GUID_WfpOutboundCallout4, FWP_ACTION_PERMIT);

						fwfc[0].conditionValue.uint8 = IPPROTO_ICMPV6;
						_wfp_createfilter (nullptr, fwfc, 1, FILTER_WEIGHT_HIGHEST, FWPM_LAYER_ALE_AUTH_CONNECT_V6, GUID_WfpOutboundCallout6, FWP_ACTION_PERMIT);
					}

					if (app.ConfigGet (L"AllowInboundIcmp", FALSE).AsBool ())
					{
						fwfc[0].conditionValue.uint8 = IPPROTO_ICMP;
						_wfp_createfilter (nullptr, fwfc, 1, FILTER_WEIGHT_HIGHEST, FWPM_LAYER_ALE_AUTH_RECV_ACCEPT_V4, GUID_WfpInboundCallout4, FWP_ACTION_PERMIT);

						fwfc[0].conditionValue.uint8 = IPPROTO_ICMPV6;
						_wfp_createfilter (nullptr, fwfc, 1, FILTER_WEIGHT_HIGHEST, FWPM_LAYER_ALE_AUTH_RECV_ACCEPT_V6, GUID_WfpInboundCallout6, FWP_ACTION_PERMIT);
					}

					// apply blocklist rules
					if (app.ConfigGet (L"UseBlocklist2", FALSE).AsBool ())
					{
						for (size_t i = 0; i < rules_blocklist.size (); i++)
						{
							if (!rules_blocklist.at (i).is_enabled)
								continue;

							const rstring arr = rules_blocklist.at (i).rule;
							rstring::rvector vc = arr.AsVector (RULE_DELIMETER);

							for (size_t j = 0; j < vc.size (); j++)
							{
								vc.at (j).Trim (L"\r\n "); // trim whitespace

								_wfp_createrulefilter (rules_blocklist.at (i).name, vc.at (j), nullptr, rules_blocklist.at (i).dir, rules_blocklist.at (i).protocol, rules_blocklist.at (i).version, rules_blocklist.at (i).is_block ? FWP_ACTION_BLOCK : FWP_ACTION_PERMIT, FILTER_WEIGHT_BLOCKLIST);
							}
						}
					}

					{
						BOOL im_allowed = FALSE;
						BOOL svchost_allowed = FALSE;
						BOOL system_allowed = FALSE;

						// apply apps rules
						{
							_R_SPINLOCK (config.lock_access);

							for (auto& p : apps)
							{
								if (!p.second.is_checked)
									continue;

								if (mode == Whitelist)
								{
									if (config.my_hash == p.first && app.ConfigGet (L"AllowInternetAccess", TRUE).AsBool ())
										im_allowed = TRUE;

									if (config.svchost_hash == p.first)
										svchost_allowed = TRUE;

									if (config.ntoskrnl_hash == p.first)
										svchost_allowed = TRUE;
								}

								p.second.is_success = _wfp_createrulefilter (p.second.file_name, nullptr, p.second.display_path, Both, 0, AF_UNSPEC, (mode == Whitelist) ? FWP_ACTION_PERMIT : FWP_ACTION_BLOCK, FILTER_WEIGHT_APPLICATION);
							}

							_R_SPINUNLOCK (config.lock_access);

							// unlock main module
							if (!im_allowed && app.ConfigGet (L"AllowInternetAccess", TRUE).AsBool ())
								_wfp_createrulefilter (nullptr, nullptr, app.GetBinaryPath (), Both, 0, AF_UNSPEC, FWP_ACTION_PERMIT, FILTER_WEIGHT_APPLICATION);
						}

						// apply system rules
						for (size_t i = 0; i < rules_system.size (); i++)
						{
							if (!rules_system.at (i).is_enabled)
								continue;

							const rstring arr = rules_system.at (i).rule;
							rstring::rvector vc = arr.AsVector (RULE_DELIMETER);

							for (size_t j = 0; j < vc.size (); j++)
							{
								vc.at (j).Trim (L"\r\n "); // trim whitespace

								// create system rules only for "svchost.exe" & "system" processes
								if (!svchost_allowed)
									_wfp_createrulefilter (rules_system.at (i).name, vc.at (j), config.svchost_path, rules_system.at (i).dir, rules_system.at (i).protocol, rules_system.at (i).version, rules_system.at (i).is_block ? FWP_ACTION_BLOCK : FWP_ACTION_PERMIT, FILTER_WEIGHT_CUSTOM);

								if (!system_allowed)
									_wfp_createrulefilter (rules_system.at (i).name, vc.at (j), PROC_SYSTEM_NAME, rules_system.at (i).dir, rules_system.at (i).protocol, rules_system.at (i).version, rules_system.at (i).is_block ? FWP_ACTION_BLOCK : FWP_ACTION_PERMIT, FILTER_WEIGHT_CUSTOM);
							}
						}

						// apply custom rules for apps
						for (auto const &p : apps_rules)
						{
							const size_t hash = p.first;

							if (apps.find (hash) != apps.end ())
							{
								for (auto const &q : p.second)
								{
									const size_t idx = q.first;

									const rstring arr = rules_custom.at (q.first).rule;
									rstring::rvector vc = arr.AsVector (RULE_DELIMETER);

									for (size_t i = 0; i < vc.size (); i++)
									{
										vc.at (i).Trim (L"\r\n "); // trim whitespace

										_wfp_createrulefilter (rules_custom.at (idx).name, vc.at (i), apps[hash].display_path, rules_custom.at (idx).dir, rules_custom.at (idx).protocol, rules_custom.at (idx).version, rules_custom.at (idx).is_block ? FWP_ACTION_BLOCK : FWP_ACTION_PERMIT, FILTER_WEIGHT_CUSTOM);
									}
								}
							}
						}

						// apply custom rules for all
						for (size_t i = 0; i < rules_custom.size (); i++)
						{
							if (!rules_custom.at (i).is_enabled)
								continue;

							const rstring arr = rules_custom.at (i).rule;
							rstring::rvector vc = arr.AsVector (RULE_DELIMETER);

							for (size_t j = 0; j < vc.size (); j++)
							{
								vc.at (j).Trim (L"\r\n "); // trim whitespace

								_wfp_createrulefilter (rules_custom.at (i).name, vc.at (j), nullptr, rules_custom.at (i).dir, rules_custom.at (i).protocol, rules_custom.at (i).version, rules_custom.at (i).is_block ? FWP_ACTION_BLOCK : FWP_ACTION_PERMIT, FILTER_WEIGHT_CUSTOM);
							}
						}
					}
				}

				// block all other traffic (only on "whitelist" & "trust no one" mode)
				if (mode == Whitelist || mode == TrustNoOne)
				{
					_wfp_createfilter (nullptr, nullptr, 0, FILTER_WEIGHT_LOWEST, FWPM_LAYER_ALE_AUTH_CONNECT_V4, GUID_WfpOutboundCallout4, FWP_ACTION_BLOCK);
					_wfp_createfilter (nullptr, nullptr, 0, FILTER_WEIGHT_LOWEST, FWPM_LAYER_ALE_AUTH_CONNECT_V6, GUID_WfpOutboundCallout6, FWP_ACTION_BLOCK);

					if (mode == TrustNoOne || app.ConfigGet (L"AllowInboundConnections", FALSE).AsBool () == FALSE)
					{
						_wfp_createfilter (nullptr, nullptr, 0, FILTER_WEIGHT_LOWEST, FWPM_LAYER_ALE_AUTH_RECV_ACCEPT_V4, GUID_WfpInboundCallout4, FWP_ACTION_BLOCK);
						_wfp_createfilter (nullptr, nullptr, 0, FILTER_WEIGHT_LOWEST, FWPM_LAYER_ALE_AUTH_RECV_ACCEPT_V6, GUID_WfpInboundCallout6, FWP_ACTION_BLOCK);

						_wfp_createfilter (nullptr, nullptr, 0, FILTER_WEIGHT_LOWEST, FWPM_LAYER_ALE_AUTH_LISTEN_V4, GUID_WfpListenCallout4, FWP_ACTION_BLOCK);
						_wfp_createfilter (nullptr, nullptr, 0, FILTER_WEIGHT_LOWEST, FWPM_LAYER_ALE_AUTH_LISTEN_V6, GUID_WfpListenCallout6, FWP_ACTION_BLOCK);
					}
				}

				// install boot-time filters (enforced at boot-time, even before "Base Filtering Engine" service starts.)
				if (app.ConfigGet (L"InstallBoottimeFilters", FALSE).AsBool ())
				{
					_wfp_createfilter (nullptr, nullptr, 0, FILTER_WEIGHT_LOWEST, FWPM_LAYER_ALE_AUTH_CONNECT_V4, GUID_WfpOutboundCallout4, FWP_ACTION_BLOCK, FWPM_FILTER_FLAG_BOOTTIME);
					_wfp_createfilter (nullptr, nullptr, 0, FILTER_WEIGHT_LOWEST, FWPM_LAYER_ALE_AUTH_CONNECT_V6, GUID_WfpOutboundCallout6, FWP_ACTION_BLOCK, FWPM_FILTER_FLAG_BOOTTIME);

					_wfp_createfilter (nullptr, nullptr, 0, FILTER_WEIGHT_LOWEST, FWPM_LAYER_ALE_AUTH_RECV_ACCEPT_V4, GUID_WfpInboundCallout4, FWP_ACTION_BLOCK, FWPM_FILTER_FLAG_BOOTTIME);
					_wfp_createfilter (nullptr, nullptr, 0, FILTER_WEIGHT_LOWEST, FWPM_LAYER_ALE_AUTH_RECV_ACCEPT_V6, GUID_WfpInboundCallout6, FWP_ACTION_BLOCK, FWPM_FILTER_FLAG_BOOTTIME);

					_wfp_createfilter (nullptr, nullptr, 0, FILTER_WEIGHT_LOWEST, FWPM_LAYER_ALE_AUTH_LISTEN_V4, GUID_WfpListenCallout4, FWP_ACTION_BLOCK, FWPM_FILTER_FLAG_BOOTTIME);
					_wfp_createfilter (nullptr, nullptr, 0, FILTER_WEIGHT_LOWEST, FWPM_LAYER_ALE_AUTH_LISTEN_V6, GUID_WfpListenCallout6, FWP_ACTION_BLOCK, FWPM_FILTER_FLAG_BOOTTIME);
				}

				FwpmTransactionCommit (config.hengine);

				// set icons
				app.SetIcon (IDI_MAIN);
				app.TraySetInfo (_r_loadicon (app.GetHINSTANCE (), MAKEINTRESOURCE (IDI_MAIN), GetSystemMetrics (SM_CXSMICON)), APP_NAME);

				SetDlgItemText (app.GetHWND (), IDC_START_BTN, I18N (&app, IDS_TRAY_STOP, 0));

				config.is_filtersinstalled = TRUE;
			}
		}
	}
}

// append log-line
VOID _app_logwrite (ITEM_LOG const* ptr)
{
	rstring buffer;
	buffer.Format (L"[%s] %s (%s\\%s) [%s:%s] (%s) %s\r\n", ptr->date, ptr->full_path, ptr->domain, ptr->username, ptr->protocol, ptr->address, ptr->name, ptr->direction);

	_R_SPINLOCK (config.lock_writelog);

	if (config.hlog != nullptr && config.hlog != INVALID_HANDLE_VALUE)
	{
		DWORD written = 0;
		WriteFile (config.hlog, buffer.GetString (), DWORD (buffer.GetLength () * sizeof (WCHAR)), &written, nullptr);
	}

	_R_SPINUNLOCK (config.lock_writelog);
}

// show dropped packet notification
VOID _app_logshownotification (ITEM_LOG const* ptr)
{
	if (apps.find (ptr->hash) != apps.end () && apps[ptr->hash].is_silent)
		return;

	if ((_r_unixtime_now () - notifications[ptr->hash]) >= (app.ConfigGet (L"NotificationsTimeout", 10).AsUint ())) // check for timeout (sec.)
	{
		app.TrayPopup (NIIF_WARNING | (app.ConfigGet (L"IsNotificationsSilent", FALSE).AsBool () ? NIIF_NOSOUND : 0), APP_NAME, _r_fmt (L"%s: %s\r\n%s: %s\r\n%s: %s (%s) [%s]\r\n%s: %s", I18N (&app, IDS_DATE, 0), ptr->date, I18N (&app, IDS_FILE, 0), ptr->full_path, I18N (&app, IDS_ADDRESS, 0), ptr->address, ptr->protocol, ptr->direction, I18N (&app, IDS_NAME, 0), ptr->name));
		notifications[ptr->hash] = _r_unixtime_now ();
		config.last_hash = ptr->hash;
	}
}

// Author: Elmue
// http://stackoverflow.com/questions/65170/how-to-get-name-associated-with-open-handle/18792477#18792477
rstring GetDosPathFromNtPath (LPCWSTR nt_path)
{
	rstring result = nt_path;

	if (_wcsnicmp (nt_path, L"\\Device\\Mup\\", 12) == 0) // Win 7
	{
		result = L"\\\\";
		result.Append (nt_path + 12);
	}
	else if (_wcsnicmp (nt_path, L"\\Device\\LanmanRedirector\\", 25) == 0) // Win XP
	{
		result = L"\\\\";
		result.Append (nt_path + 25);
	}
	else
	{
		WCHAR drives[128] = {0};

		if (GetLogicalDriveStrings (_countof (drives), drives))
		{
			LPWSTR drv = drives;

			while (drv[0])
			{
				LPWSTR drv_next = drv + wcslen (drv) + 1;

				drv[2] = 0; // the backslash is not allowed for QueryDosDevice()

				WCHAR u16_NtVolume[1000];
				u16_NtVolume[0] = 0;

				// may return multiple strings!
				// returns very weird strings for network shares
				if (QueryDosDevice (drv, u16_NtVolume, sizeof (u16_NtVolume) / 2))
				{
					size_t s32_Len = wcslen (u16_NtVolume);
					if (s32_Len > 0 && _wcsnicmp (nt_path, u16_NtVolume, s32_Len) == 0)
					{
						result = drv;
						result.Append (nt_path + s32_Len);

						break;
					}
				}

				drv = drv_next;
			}
		}
	}

	return result;
}

VOID CALLBACK _app_logcallback (LPVOID, const FWPM_NET_EVENT1* pEvent)
{
	if (!pEvent || ((pEvent->header.flags & FWPM_NET_EVENT_FLAG_APP_ID_SET) == 0) || !pEvent->header.appId.data)
		return;

	const BOOL is_logenabled = app.ConfigGet (L"IsLogEnabled", FALSE).AsBool ();
	const BOOL is_notificationenabled = app.ConfigGet (L"IsNotificationsEnabled", TRUE).AsBool ();
	const BOOL is_collectorenabled = app.ConfigGet (L"IsAppsCollectorEnabled", TRUE).AsBool ();

	if (!is_logenabled && !is_notificationenabled && !is_collectorenabled)
		return;

	ITEM_LOG log;
	SecureZeroMemory (&log, sizeof (log));

	// copy date and time
	log.timestamp = _r_unixtime_from_filetime (&pEvent->header.timeStamp);
	StringCchCopy (log.date, _countof (log.date), _r_fmt_date (log.timestamp, FDTF_SHORTDATE | FDTF_LONGTIME));

	// copy converted nt device path into win32
	rstring path = GetDosPathFromNtPath (LPCWSTR (pEvent->header.appId.data));
	log.hash = path.Hash ();
	StringCchCopy (log.full_path, _countof (log.full_path), path);

	// get username & domain
	if (pEvent->header.userId)
	{
		SID_NAME_USE sid_type;
		SecureZeroMemory (&sid_type, sizeof (sid_type));

		DWORD length1 = _countof (log.username);
		DWORD length2 = _countof (log.domain);

		LookupAccountSid (nullptr, pEvent->header.userId, log.username, &length1, log.domain, &length2, &sid_type);
	}

	if (pEvent->classifyDrop)
	{
		// read filter information
		if (pEvent->classifyDrop->filterId)
		{
			FWPM_FILTER* filter = nullptr;
			DWORD result = FwpmFilterGetById (config.hengine, pEvent->classifyDrop->filterId, &filter);

			if (result == ERROR_SUCCESS)
			{
				StringCchCopy (log.name, _countof (log.name), filter->displayData.description);

				FwpmFreeMemory ((LPVOID*)&filter);
			}
			else
			{
				StringCchPrintf (log.name, _countof (log.name), L"Filter #%d", pEvent->classifyDrop->filterId);
			}
		}
	}

	// protocol
	if ((pEvent->header.flags & FWPM_NET_EVENT_FLAG_IP_PROTOCOL_SET) != 0)
	{
		for (size_t i = 0; i < protocols.size (); i++)
		{
			if (protocols.at (i).v == pEvent->header.ipProtocol)
				StringCchCopy (log.protocol, _countof (log.protocol), protocols.at (i).t);
		}

		if (!log.protocol[0])
			StringCchCopy (log.protocol, _countof (log.protocol), NA_TEXT);
	}

	// ipv4 address
	if (pEvent->header.ipVersion == FWP_IP_VERSION_V4 && (is_logenabled || is_notificationenabled))
	{
		if ((pEvent->header.flags & FWPM_NET_EVENT_FLAG_LOCAL_ADDR_SET) != 0 || (pEvent->header.flags & FWPM_NET_EVENT_FLAG_REMOTE_ADDR_SET) != 0)
		{
			if ((pEvent->header.flags & FWPM_NET_EVENT_FLAG_REMOTE_ADDR_SET) != 0)
			{
				StringCchCopy (log.direction, _countof (log.direction), L"Remote"); // remote address

				StringCchPrintf (log.address, _countof (log.address), L"%d.%d.%d.%d",
					pEvent->header.remoteAddrV6.byteArray16[3],
					pEvent->header.remoteAddrV6.byteArray16[2],
					pEvent->header.remoteAddrV6.byteArray16[1],
					pEvent->header.remoteAddrV6.byteArray16[0]
				);

				if ((pEvent->header.flags & FWPM_NET_EVENT_FLAG_REMOTE_PORT_SET) != 0)
					StringCchCat (log.address, _countof (log.address), _r_fmt (L":%d", pEvent->header.remotePort));

				if (is_logenabled)
					_app_logwrite (&log);

				if (is_notificationenabled)
					_app_logshownotification (&log);
			}

			if ((pEvent->header.flags & FWPM_NET_EVENT_FLAG_LOCAL_ADDR_SET) != 0)
			{
				StringCchCopy (log.direction, _countof (log.direction), L"Local"); // local address

				StringCchPrintf (log.address, _countof (log.address), L"%d.%d.%d.%d",
					pEvent->header.localAddrV6.byteArray16[3],
					pEvent->header.localAddrV6.byteArray16[2],
					pEvent->header.localAddrV6.byteArray16[1],
					pEvent->header.localAddrV6.byteArray16[0]
				);

				if ((pEvent->header.flags & FWPM_NET_EVENT_FLAG_LOCAL_PORT_SET) != 0)
					StringCchCat (log.address, _countof (log.address), _r_fmt (L":%d", pEvent->header.localPort));

				if (is_logenabled)
					_app_logwrite (&log);

				if (is_notificationenabled)
					_app_logshownotification (&log);
			}
		}
	}
	else if (pEvent->header.ipVersion == FWP_IP_VERSION_V6 && (is_logenabled || is_notificationenabled))
	{
		if ((pEvent->header.flags & FWPM_NET_EVENT_FLAG_LOCAL_ADDR_SET) != 0 || (pEvent->header.flags & FWPM_NET_EVENT_FLAG_REMOTE_ADDR_SET) != 0)
		{
			if ((pEvent->header.flags & FWPM_NET_EVENT_FLAG_LOCAL_ADDR_SET) != 0)
			{
				StringCchCopy (log.direction, _countof (log.direction), L"Remote"); // remote address

				StringCchPrintf (log.address, _countof (log.address), L"%02x%02x:%02x%02x:%02x%02x:%02x%02x:%02x%02x:%02x%02x:%02x%02x:%02x%02x",
					pEvent->header.remoteAddrV6.byteArray16[0],
					pEvent->header.remoteAddrV6.byteArray16[1],
					pEvent->header.remoteAddrV6.byteArray16[2],
					pEvent->header.remoteAddrV6.byteArray16[3],
					pEvent->header.remoteAddrV6.byteArray16[4],
					pEvent->header.remoteAddrV6.byteArray16[5],
					pEvent->header.remoteAddrV6.byteArray16[6],
					pEvent->header.remoteAddrV6.byteArray16[7],
					pEvent->header.remoteAddrV6.byteArray16[8],
					pEvent->header.remoteAddrV6.byteArray16[9],
					pEvent->header.remoteAddrV6.byteArray16[10],
					pEvent->header.remoteAddrV6.byteArray16[11],
					pEvent->header.remoteAddrV6.byteArray16[12],
					pEvent->header.remoteAddrV6.byteArray16[13],
					pEvent->header.remoteAddrV6.byteArray16[14],
					pEvent->header.remoteAddrV6.byteArray16[15]
				);

				if ((pEvent->header.flags & FWPM_NET_EVENT_FLAG_REMOTE_PORT_SET) != 0)
					StringCchCat (log.address, _countof (log.address), _r_fmt (L":%d", pEvent->header.remotePort));

				if (is_logenabled)
					_app_logwrite (&log);

				if (is_notificationenabled)
					_app_logshownotification (&log);
			}

			if ((pEvent->header.flags & FWPM_NET_EVENT_FLAG_LOCAL_ADDR_SET) != 0)
			{
				StringCchCopy (log.direction, _countof (log.direction), L"Local"); // local address

				StringCchPrintf (log.address, _countof (log.address), L"%02x%02x:%02x%02x:%02x%02x:%02x%02x:%02x%02x:%02x%02x:%02x%02x:%02x%02x",
					pEvent->header.localAddrV6.byteArray16[0],
					pEvent->header.localAddrV6.byteArray16[1],
					pEvent->header.localAddrV6.byteArray16[2],
					pEvent->header.localAddrV6.byteArray16[3],
					pEvent->header.localAddrV6.byteArray16[4],
					pEvent->header.localAddrV6.byteArray16[5],
					pEvent->header.localAddrV6.byteArray16[6],
					pEvent->header.localAddrV6.byteArray16[7],
					pEvent->header.localAddrV6.byteArray16[8],
					pEvent->header.localAddrV6.byteArray16[9],
					pEvent->header.localAddrV6.byteArray16[10],
					pEvent->header.localAddrV6.byteArray16[11],
					pEvent->header.localAddrV6.byteArray16[12],
					pEvent->header.localAddrV6.byteArray16[13],
					pEvent->header.localAddrV6.byteArray16[14],
					pEvent->header.localAddrV6.byteArray16[15]
				);

				if ((pEvent->header.flags & FWPM_NET_EVENT_FLAG_LOCAL_PORT_SET) != 0)
					StringCchCat (log.address, _countof (log.address), _r_fmt (L":%d", pEvent->header.localPort));

				if (is_logenabled)
					_app_logwrite (&log);

				if (is_notificationenabled)
					_app_logshownotification (&log);
			}
		}
	}

	// apps collector
	if (is_collectorenabled)
	{
		if (apps.find (log.hash) == apps.end ())
		{
			_app_addapplication (app.GetHWND (), log.full_path, 0, FALSE);

			_app_listviewsort (app.GetHWND ());
			_app_profilesave (app.GetHWND ());
		}
	}
}

VOID _app_loginit (BOOL is_install)
{
	if (!_r_sys_validversion (6, 1))
		return;

	// reset all handles
	_R_SPINLOCK (config.lock_writelog);

	if (config.hlog != nullptr && config.hlog != INVALID_HANDLE_VALUE)
	{
		CloseHandle (config.hlog);
		config.hlog = nullptr;
	}

	_R_SPINUNLOCK (config.lock_writelog);

	if (!is_install)
		return; // already closed

	// check if log enabled
	if (!app.ConfigGet (L"IsLogEnabled", FALSE).AsBool ())
		return;

	if (is_install)
	{
		config.hlog = CreateFile (_r_path_expand (app.ConfigGet (L"LogPath", PATH_LOG)), GENERIC_WRITE, FILE_SHARE_READ, nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL | FILE_FLAG_WRITE_THROUGH, nullptr);

		if (config.hlog != INVALID_HANDLE_VALUE)
		{
			_R_SPINLOCK (config.lock_writelog);

			if (GetLastError () != ERROR_ALREADY_EXISTS)
			{
				DWORD written = 0;
				static const BYTE bom[] = {0xFF, 0xFE};

				WriteFile (config.hlog, bom, sizeof (bom), &written, nullptr); // write utf-16 le byte order mask
			}
			else
			{
				SetFilePointer (config.hlog, 0, nullptr, FILE_END);
			}

			_R_SPINUNLOCK (config.lock_writelog);
		}
		else
		{
			WDBG (L"CreateFile failed. Return value: 0x%.8lx.", GetLastError ());
		}
	}
}

UINT WINAPI ApplyThread (LPVOID)
{
	const HANDLE evts[] = {config.stop_evt, config.install_evt, config.destroy_evt};

	while (TRUE)
	{
		const DWORD state = WaitForMultipleObjectsEx (_countof (evts), evts, FALSE, INFINITE, FALSE);

		if (state == WAIT_OBJECT_0) // stop event
		{
			break;
		}
		else if (state == WAIT_OBJECT_0 + 1) // install filters event
		{
			_R_SPINLOCK (config.lock_apply);

			if (app.IsAdmin () && app.ConfigGet (L"IsFiltersEnabled", FALSE).AsBool ())
				_wfp_installfilters ();

			_app_listviewsort (app.GetHWND ());
			_app_profilesave (app.GetHWND ());

			_R_SPINUNLOCK (config.lock_apply);
		}
		else if (state == WAIT_OBJECT_0 + 2) // destroy filters event
		{
			_R_SPINLOCK (config.lock_apply);

			if (app.IsAdmin ())
				_wfp_destroyfilters (TRUE);

			_R_SPINUNLOCK (config.lock_apply);
		}
		else
		{
			break;
		}
	}

	return ERROR_SUCCESS;
}

VOID addcolor (LPCWSTR locale_sid, UINT locale_id, LPCWSTR cfg, BOOL is_enabled, LPCWSTR config_color, COLORREF default_clr)
{
	ITEM_COLOR color;
	SecureZeroMemory (&color, sizeof (color));

	StringCchCopy (color.config, _countof (color.config), cfg);
	StringCchCopy (color.config_color, _countof (color.config_color), config_color);

	color.is_enabled = is_enabled;
	color.default_clr = default_clr;

	color.locale_id = locale_id;
	StringCchCopy (color.locale_sid, _countof (color.locale_sid), locale_sid);

	colors.push_back (color);
}

VOID addprotocol (LPCWSTR n, UINT8 v)
{
	ITEM_PROTOCOL protocol;
	SecureZeroMemory (&protocol, sizeof (protocol));

	protocol.v = v;
	StringCchCopy (protocol.t, _countof (protocol.t), n);

	protocols.push_back (protocol);
}

HBITMAP _app_ico2bmp (HICON hico)
{
	const INT icon_size = GetSystemMetrics (SM_CXSMICON);

	RECT rc = {0};
	rc.right = icon_size;
	rc.bottom = icon_size;

	HDC hdc = GetDC (nullptr);
	HDC hmemdc = CreateCompatibleDC (hdc);
	HBITMAP hbitmap = CreateCompatibleBitmap (hdc, icon_size, icon_size);
	ReleaseDC (nullptr, hdc);

	HGDIOBJ old_bmp = SelectObject (hmemdc, hbitmap);
	_r_wnd_fillrect (hmemdc, &rc, GetSysColor (COLOR_MENU));
	DrawIconEx (hmemdc, 0, 0, hico, icon_size, icon_size, 0, nullptr, DI_NORMAL);
	SelectObject (hmemdc, old_bmp);

	DeleteDC (hmemdc);

	return hbitmap;
}

VOID _app_getprocesslist (std::vector<ITEM_PROCESS>* pvc)
{
	if (!pvc)
		return;

	// clear previous result
	{
		for (size_t i = 0; i < pvc->size (); i++)
		{
			if (pvc->at (i).hbmp)
				DeleteObject (pvc->at (i).hbmp); // free memory
		}

		pvc->clear ();
	}

	NTSTATUS status = 0;

	ULONG length = 0x4000;
	PVOID buffer = malloc (length);

	while (TRUE)
	{
		status = NtQuerySystemInformation (SystemProcessInformation, buffer, length, &length);

		if (status == 0xC0000023L /*STATUS_BUFFER_TOO_SMALL*/ || status == 0xc0000004 /*STATUS_INFO_LENGTH_MISMATCH*/)
		{
			PVOID buffer_new = realloc (buffer, length);

			if (!buffer_new)
			{
				break;
			}
			else
			{
				buffer = buffer_new;
			}
		}
		else
		{
			break;
		}
	}

	if (!NT_SUCCESS (status))
	{
		WDBG (L"NtQuerySystemInformation failed. Return value: 0x%.8lx.", status);
	}
	else
	{
		PSYSTEM_PROCESS_INFORMATION spi = (PSYSTEM_PROCESS_INFORMATION)buffer;

		std::unordered_map<size_t, BOOL> checker;

		do
		{
			DWORD pid = (DWORD)(DWORD_PTR)spi->UniqueProcessId;

			if (!pid) // skip "system idle process" with 0 pid
				continue;

			HANDLE hprocess = OpenProcess (PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);

			if (!hprocess)
			{
				WDBG (L"OpenProcess failed. Return value: 0x%.8lx. (%s)", GetLastError (), spi->ImageName.Buffer);
			}
			else
			{
				WCHAR display_path[64] = {0};
				WCHAR real_path[MAX_PATH] = {0};

				DWORD size = _countof (real_path) - 1;
				size_t hash = 0;

				StringCchPrintf (display_path, _countof (display_path), L"%s (%d)", spi->ImageName.Buffer, pid);

				if (pid == PROC_SYSTEM_PID)
				{
					StringCchCopy (real_path, _countof (real_path), _r_path_expand (PATH_NTOSKRNL));

					hash = _r_str_hash (spi->ImageName.Buffer);
				}
				else if (QueryFullProcessImageName (hprocess, 0, real_path, &size))
				{
					hash = _r_str_hash (real_path);
				}
				else
				{
					WDBG (L"QueryFullProcessImageName failed. Return value: 0x%.8lx. (%s)", GetLastError (), spi->ImageName.Buffer);

					CloseHandle (hprocess);
					continue;
				}

				if (hash && apps.find (hash) == apps.end () && checker.find (hash) == checker.end ())
				{
					checker[hash] = TRUE;

					ITEM_PROCESS item;
					SecureZeroMemory (&item, sizeof (item));

					StringCchCopy (item.display_path, _countof (item.display_path), display_path);
					StringCchCopy (item.file_path, _countof (item.file_path), ((pid == PROC_SYSTEM_PID) ? PROC_SYSTEM_NAME : real_path));

					SHFILEINFO shfi = {0};
					SHGetFileInfo (real_path, 0, &shfi, sizeof (shfi), SHGFI_SMALLICON | SHGFI_ICON);

					item.hbmp = _app_ico2bmp (shfi.hIcon);

					DestroyIcon (shfi.hIcon);

					pvc->push_back (item);
				}

				CloseHandle (hprocess);
			}
		}
		while ((spi = ((spi->NextEntryOffset ? (PSYSTEM_PROCESS_INFORMATION)((PCHAR)(spi)+(spi)->NextEntryOffset) : nullptr))) != nullptr);
	}

	free (buffer); // free the allocated buffer
}

VOID _app_profileload (HWND hwnd)
{
	// load applications
	{
		_R_SPINLOCK (config.lock_access);

		apps.clear ();

		_R_SPINUNLOCK (config.lock_access);

		_r_listview_deleteallitems (hwnd, IDC_LISTVIEW);

		pugi::xml_document doc;

		if (doc.load_file (config.apps_path, pugi::parse_default, pugi::encoding_auto))
		{
			pugi::xml_node root = doc.child (L"root");

			if (root)
			{
				for (pugi::xml_node item = root.child (L"item"); item; item = item.next_sibling (L"item"))
				{
					_app_addapplication (hwnd, item.attribute (L"path").as_string (), item.attribute (L"is_silent").as_bool (), item.attribute (L"is_enabled").as_bool ());
				}
			}
		}
	}

	// load rules
	_app_loadrules (hwnd, config.blocklist_path, SECTION_BLOCKLIST, &rules_blocklist);
	_app_loadrules (hwnd, config.rules_system_path, SECTION_SYSTEM, &rules_system);
	_app_loadrules (hwnd, config.rules_custom_path, nullptr, &rules_custom);

	// set default colors
	{
		colors.clear ();

		addcolor (L"IDS_HIGHLIGHT_CUSTOM", IDS_HIGHLIGHT_CUSTOM, L"IsHighlightCustom", TRUE, L"ColorCustom", LISTVIEW_COLOR_CUSTOM);
		addcolor (L"IDS_HIGHLIGHT_SYSTEM", IDS_HIGHLIGHT_SYSTEM, L"IsHighlightSystem", TRUE, L"ColorSystem", LISTVIEW_COLOR_SYSTEM);
		addcolor (L"IDS_HIGHLIGHT_NETWORK", IDS_HIGHLIGHT_NETWORK, L"IsHighlightNetwork", TRUE, L"ColorNetwork", LISTVIEW_COLOR_NETWORK);
		addcolor (L"IDS_HIGHLIGHT_INVALID", IDS_HIGHLIGHT_INVALID, L"IsHighlightInvalid", TRUE, L"ColorInvalid", LISTVIEW_COLOR_INVALID);
		addcolor (L"IDS_HIGHLIGHT_SILENT", IDS_HIGHLIGHT_SILENT, L"IsHighlightSilent", TRUE, L"ColorSilent", LISTVIEW_COLOR_SILENT);
	}

	// set of protocols
	{
		protocols.clear ();

		addprotocol (L"ICMP", IPPROTO_ICMP);
		addprotocol (L"ICMPv6", IPPROTO_ICMPV6);
		addprotocol (L"IGMP", IPPROTO_IGMP);
		addprotocol (L"IPv4", IPPROTO_IPV4);
		addprotocol (L"IPv6", IPPROTO_IPV6);
		addprotocol (L"L2TP", IPPROTO_L2TP);
		addprotocol (L"RAW", IPPROTO_RAW);
		addprotocol (L"RDP", IPPROTO_RDP);
		addprotocol (L"SCTP", IPPROTO_SCTP);
		addprotocol (L"TCP", IPPROTO_TCP);
		addprotocol (L"UDP", IPPROTO_UDP);
	}
}

BOOL initializer_callback (HWND hwnd, DWORD msg, LPVOID, LPVOID)
{
	switch (msg)
	{
		case _RM_INITIALIZE:
		{
			// set icons
			app.SetIcon (config.is_filtersinstalled ? IDI_MAIN : IDI_INACTIVE);
			app.TrayCreate (hwnd, UID, WM_TRAYICON, _r_loadicon (app.GetHINSTANCE (), MAKEINTRESOURCE (config.is_filtersinstalled ? IDI_MAIN : IDI_INACTIVE), GetSystemMetrics (SM_CXSMICON)), FALSE);

			// load profile
			_app_profileload (hwnd);
			_app_listviewsort (hwnd);

			_app_loginit (TRUE); // enable dropped packets logging (win7 and above)

			if (app.ConfigGet (L"IsFiltersEnabled", FALSE).AsBool ())
				SetEvent (config.install_evt); // apply filters

			CheckMenuItem (GetMenu (hwnd), IDM_ALWAYSONTOP_CHK, MF_BYCOMMAND | (app.ConfigGet (L"AlwaysOnTop", FALSE).AsBool () ? MF_CHECKED : MF_UNCHECKED));
			CheckMenuItem (GetMenu (hwnd), IDM_STARTMINIMIZED_CHK, MF_BYCOMMAND | (app.ConfigGet (L"StartMinimized", FALSE).AsBool () ? MF_CHECKED : MF_UNCHECKED));
			CheckMenuItem (GetMenu (hwnd), IDM_SHOWFILENAMESONLY_CHK, MF_BYCOMMAND | (app.ConfigGet (L"ShowFilenames", TRUE).AsBool () ? MF_CHECKED : MF_UNCHECKED));

			CheckMenuRadioItem (GetMenu (hwnd), IDM_TRAY_MODEWHITELIST, IDM_TRAY_MODETRUSTNOONE, IDM_TRAY_MODEWHITELIST + app.ConfigGet (L"Mode", Whitelist).AsUint (), MF_BYCOMMAND);

			CheckMenuItem (GetMenu (hwnd), IDM_USEBLOCKLIST_CHK, MF_BYCOMMAND | (app.ConfigGet (L"UseBlocklist2", FALSE).AsBool () ? MF_CHECKED : MF_UNCHECKED));
			CheckMenuItem (GetMenu (hwnd), IDM_INSTALLBOOTTIMEFILTERS_CHK, MF_BYCOMMAND | (app.ConfigGet (L"InstallBoottimeFilters", FALSE).AsBool () ? MF_CHECKED : MF_UNCHECKED));
			CheckMenuItem (GetMenu (hwnd), IDM_USEUPDATECHECKING_CHK, MF_BYCOMMAND | (app.ConfigGet (L"AllowInternetAccess", TRUE).AsBool () ? MF_CHECKED : MF_UNCHECKED));

			CheckMenuItem (GetMenu (hwnd), IDM_RULE_OUTBOUND_ICMP, MF_BYCOMMAND | (app.ConfigGet (L"AllowOutboundIcmp", TRUE).AsBool () ? MF_CHECKED : MF_UNCHECKED));
			CheckMenuItem (GetMenu (hwnd), IDM_RULE_INBOUND_ICMP, MF_BYCOMMAND | (app.ConfigGet (L"AllowInboundIcmp", FALSE).AsBool () ? MF_CHECKED : MF_UNCHECKED));
			CheckMenuItem (GetMenu (hwnd), IDM_RULE_INBOUND, MF_BYCOMMAND | (app.ConfigGet (L"AllowInboundConnections", FALSE).AsBool () ? MF_CHECKED : MF_UNCHECKED));

			// append system rules
			{
				HMENU submenu = GetSubMenu (GetSubMenu (GetMenu (hwnd), 3), 3);

				// clear menu
				for (UINT i = 0;; i++)
				{
					if (!DeleteMenu (submenu, IDM_RULES_SYSTEM + i, MF_BYCOMMAND))
					{
						DeleteMenu (submenu, 0, MF_BYPOSITION); // delete separator
						break;
					}
				}

				if (rules_system.empty ())
				{
					AppendMenu (submenu, MF_STRING, IDM_RULES_SYSTEM, I18N (&app, IDS_STATUS_EMPTY2, 0));
					EnableMenuItem (submenu, IDM_RULES_SYSTEM, MF_BYCOMMAND | MF_DISABLED);
				}
				else
				{
					for (size_t i = 0; i < rules_system.size (); i++)
					{
						WCHAR buffer[128] = {0};
						StringCchPrintf (buffer, _countof (buffer), L"[%s] %s", rules_system.at (i).is_block ? I18N (&app, IDS_ACTION_2, 0) : I18N (&app, IDS_ACTION_1, 0), rules_system.at (i).name);

						AppendMenu (submenu, MF_STRING, IDM_RULES_SYSTEM + i, buffer);

						if (app.ConfigGet (rules_system.at (i).name, rules_system.at (i).is_enabled, SECTION_SYSTEM).AsBool ())
							CheckMenuItem (submenu, IDM_RULES_SYSTEM + UINT (i), MF_BYCOMMAND | MF_CHECKED);
					}
				}
			}

			// append custom rules
			{
				HMENU submenu = GetSubMenu (GetSubMenu (GetMenu (hwnd), 3), 4);

				// clear menu
				for (UINT i = 0;; i++)
				{
					if (!DeleteMenu (submenu, IDM_RULES_CUSTOM + i, MF_BYCOMMAND))
					{
						DeleteMenu (submenu, 0, MF_BYPOSITION); // delete separator
						break;
					}
				}

				if (rules_custom.empty ())
				{
					AppendMenu (submenu, MF_STRING, IDM_RULES_CUSTOM, I18N (&app, IDS_STATUS_EMPTY2, 0));
					EnableMenuItem (submenu, IDM_RULES_CUSTOM, MF_BYCOMMAND | MF_DISABLED);
				}
				else
				{
					for (size_t i = 0; i < rules_custom.size (); i++)
					{
						WCHAR buffer[128] = {0};
						StringCchPrintf (buffer, _countof (buffer), L"[%s] %s", rules_custom.at (i).is_block ? I18N (&app, IDS_ACTION_2, 0) : I18N (&app, IDS_ACTION_1, 0), rules_custom.at (i).name);

						AppendMenu (submenu, MF_STRING, IDM_RULES_CUSTOM + i, buffer);

						if (rules_custom.at (i).is_enabled)
							CheckMenuItem (submenu, IDM_RULES_CUSTOM + UINT (i), MF_BYCOMMAND | MF_CHECKED);
					}
				}
			}

			CheckMenuItem (GetMenu (hwnd), IDM_ENABLELOG_CHK, MF_BYCOMMAND | (app.ConfigGet (L"IsLogEnabled", FALSE).AsBool () ? MF_CHECKED : MF_UNCHECKED));
			CheckMenuItem (GetMenu (hwnd), IDM_ENABLENOTIFICATIONS_CHK, MF_BYCOMMAND | (app.ConfigGet (L"IsNotificationsEnabled", TRUE).AsBool () ? MF_CHECKED : MF_UNCHECKED));
			CheckMenuItem (GetMenu (hwnd), IDM_ENABLEAPPSCOLLECTOR_CHK, MF_BYCOMMAND | (app.ConfigGet (L"IsAppsCollectorEnabled", TRUE).AsBool () ? MF_CHECKED : MF_UNCHECKED));

			if (!app.IsAdmin () || !_r_sys_validversion (6, 1))
			{
				EnableMenuItem (GetMenu (hwnd), IDM_ENABLELOG_CHK, MF_BYCOMMAND | MF_DISABLED);
				EnableMenuItem (GetMenu (hwnd), IDM_ENABLENOTIFICATIONS_CHK, MF_BYCOMMAND | MF_DISABLED);
				EnableMenuItem (GetMenu (hwnd), IDM_ENABLEAPPSCOLLECTOR_CHK, MF_BYCOMMAND | MF_DISABLED);
			}

			break;
		}

		case _RM_LOCALIZE:
		{
			HMENU menu = GetMenu (hwnd);

			app.LocaleMenu (menu, I18N (&app, IDS_FILE, 0), 0, TRUE);
			app.LocaleMenu (menu, I18N (&app, IDS_SETTINGS, 0) + L"\tCtrl+P", IDM_SETTINGS, FALSE);
			app.LocaleMenu (menu, I18N (&app, IDS_EXIT, 0) + L"\tAlt+F4", IDM_EXIT, FALSE);

			app.LocaleMenu (menu, I18N (&app, IDS_EDIT, 0), 1, TRUE);

			app.LocaleMenu (menu, I18N (&app, IDS_PURGEN, 0) + L"\tCtrl+Del", IDM_PURGEN, FALSE);

			app.LocaleMenu (menu, I18N (&app, IDS_FIND, 0) + L"\tCtrl+F", IDM_FIND, FALSE);
			app.LocaleMenu (menu, I18N (&app, IDS_FINDNEXT, 0) + L"\tF3", IDM_FINDNEXT, FALSE);

			app.LocaleMenu (menu, I18N (&app, IDS_REFRESH, 0) + L"\tF5", IDM_REFRESH, FALSE);

			app.LocaleMenu (menu, I18N (&app, IDS_VIEW, 0), 2, TRUE);

			app.LocaleMenu (menu, I18N (&app, IDS_ALWAYSONTOP_CHK, 0), IDM_ALWAYSONTOP_CHK, FALSE);
			app.LocaleMenu (menu, I18N (&app, IDS_STARTMINIMIZED_CHK, 0), IDM_STARTMINIMIZED_CHK, FALSE);
			app.LocaleMenu (menu, I18N (&app, IDS_SHOWFILENAMESONLY_CHK, 0), IDM_SHOWFILENAMESONLY_CHK, FALSE);

			app.LocaleMenu (GetSubMenu (menu, 2), I18N (&app, IDS_ICONS, 0), 4, TRUE);
			app.LocaleMenu (menu, I18N (&app, IDS_ICONSSMALL, 0), IDM_ICONSSMALL, FALSE);
			app.LocaleMenu (menu, I18N (&app, IDS_ICONSLARGE, 0), IDM_ICONSLARGE, FALSE);
			app.LocaleMenu (menu, I18N (&app, IDS_ICONSISHIDDEN, 0), IDM_ICONSISHIDDEN, FALSE);

			app.LocaleMenu (GetSubMenu (menu, 2), I18N (&app, IDS_SORT, 0), 5, TRUE);
			app.LocaleMenu (menu, I18N (&app, IDS_SORTBYFNAME, 0), IDM_SORTBYFNAME, FALSE);
			app.LocaleMenu (menu, I18N (&app, IDS_SORTBYFDIR, 0), IDM_SORTBYFDIR, FALSE);
			app.LocaleMenu (menu, I18N (&app, IDS_SORTISDESCEND, 0), IDM_SORTISDESCEND, FALSE);

			app.LocaleMenu (GetSubMenu (menu, 2), I18N (&app, IDS_LANGUAGE, 0), 7, TRUE);

			app.LocaleMenu (menu, I18N (&app, IDS_SETTINGS, 0), 3, TRUE);

			app.LocaleMenu (GetSubMenu (menu, 3), I18N (&app, IDS_TRAY_MODE, 0), 0, TRUE);

			app.LocaleMenu (menu, I18N (&app, IDS_MODE_WHITELIST, 0), IDM_TRAY_MODEWHITELIST, FALSE);
			app.LocaleMenu (menu, I18N (&app, IDS_MODE_BLACKLIST, 0), IDM_TRAY_MODEBLACKLIST, FALSE);
			app.LocaleMenu (menu, I18N (&app, IDS_MODE_TRUSTNOONE, 0), IDM_TRAY_MODETRUSTNOONE, FALSE);

			app.LocaleMenu (GetSubMenu (menu, 3), I18N (&app, IDS_TRAY_FILTERS, 0), 2, TRUE);

			app.LocaleMenu (menu, I18N (&app, IDS_USEBLOCKLIST_CHK, 0), IDM_USEBLOCKLIST_CHK, FALSE);
			app.LocaleMenu (menu, I18N (&app, IDS_INSTALLBOOTTIMEFILTERS_CHK, 0), IDM_INSTALLBOOTTIMEFILTERS_CHK, FALSE);
			app.LocaleMenu (menu, _r_fmt (I18N (&app, IDS_USEUPDATECHECKING_CHK, 0), APP_NAME), IDM_USEUPDATECHECKING_CHK, FALSE);

			app.LocaleMenu (menu, I18N (&app, IDS_RULE_OUTBOUND_ICMP, 0), IDM_RULE_OUTBOUND_ICMP, FALSE);
			app.LocaleMenu (menu, I18N (&app, IDS_RULE_INBOUND_ICMP, 0), IDM_RULE_INBOUND_ICMP, FALSE);
			app.LocaleMenu (menu, I18N (&app, IDS_RULE_INBOUND, 0), IDM_RULE_INBOUND, FALSE);

			app.LocaleMenu (GetSubMenu (menu, 3), I18N (&app, IDS_TRAY_SYSTEM_RULES, 0), 3, TRUE);
			app.LocaleMenu (GetSubMenu (menu, 3), I18N (&app, IDS_TRAY_CUSTOM_RULES, 0), 4, TRUE);
			app.LocaleMenu (GetSubMenu (menu, 3), I18N (&app, IDS_TRAY_LOG, 0), 6, TRUE);

			app.LocaleMenu (menu, I18N (&app, IDS_ENABLELOG_CHK, 0), IDM_ENABLELOG_CHK, FALSE);
			app.LocaleMenu (menu, I18N (&app, IDS_ENABLENOTIFICATIONS_CHK, 0), IDM_ENABLENOTIFICATIONS_CHK, FALSE);
			app.LocaleMenu (menu, I18N (&app, IDS_ENABLEAPPSCOLLECTOR_CHK, 0), IDM_ENABLEAPPSCOLLECTOR_CHK, FALSE);
			app.LocaleMenu (menu, I18N (&app, IDS_LOGSHOW, 0) + L"\tCtrl+I", IDM_LOGSHOW, FALSE);
			app.LocaleMenu (menu, I18N (&app, IDS_LOGCLEAR, 0) + L"\tCtrl+X", IDM_LOGCLEAR, FALSE);

			// append system rules
			{
				HMENU submenu = GetSubMenu (GetSubMenu (GetMenu (hwnd), 3), 3);

				// clear menu
				for (UINT i = 0;; i++)
				{
					if (!DeleteMenu (submenu, IDM_RULES_SYSTEM + i, MF_BYCOMMAND))
					{
						DeleteMenu (submenu, 0, MF_BYPOSITION); // delete separator
						break;
					}
				}

				if (rules_system.empty ())
				{
					AppendMenu (submenu, MF_STRING, IDM_RULES_SYSTEM, I18N (&app, IDS_STATUS_EMPTY2, 0));
					EnableMenuItem (submenu, IDM_RULES_SYSTEM, MF_BYCOMMAND | MF_DISABLED);
				}
				else
				{
					for (size_t i = 0; i < rules_system.size (); i++)
					{
						WCHAR buffer[128] = {0};
						StringCchPrintf (buffer, _countof (buffer), L"[%s] %s", rules_system.at (i).is_block ? I18N (&app, IDS_ACTION_2, 0) : I18N (&app, IDS_ACTION_1, 0), rules_system.at (i).name);

						AppendMenu (submenu, MF_STRING, IDM_RULES_SYSTEM + i, buffer);

						if (app.ConfigGet (rules_system.at (i).name, rules_system.at (i).is_enabled, SECTION_SYSTEM).AsBool ())
							CheckMenuItem (submenu, IDM_RULES_SYSTEM + UINT (i), MF_BYCOMMAND | MF_CHECKED);
					}
				}
			}

			// append custom rules
			{
				HMENU submenu = GetSubMenu (GetSubMenu (GetMenu (hwnd), 3), 4);

				// clear menu
				for (UINT i = 0;; i++)
				{
					if (!DeleteMenu (submenu, IDM_RULES_CUSTOM + i, MF_BYCOMMAND))
					{
						DeleteMenu (submenu, 0, MF_BYPOSITION); // delete separator
						break;
					}
				}

				if (rules_custom.empty ())
				{
					AppendMenu (submenu, MF_STRING, IDM_RULES_CUSTOM, I18N (&app, IDS_STATUS_EMPTY2, 0));
					EnableMenuItem (submenu, IDM_RULES_CUSTOM, MF_BYCOMMAND | MF_DISABLED);
				}
				else
				{
					for (size_t i = 0; i < rules_custom.size (); i++)
					{
						WCHAR buffer[128] = {0};
						StringCchPrintf (buffer, _countof (buffer), L"[%s] %s", rules_custom.at (i).is_block ? I18N (&app, IDS_ACTION_2, 0) : I18N (&app, IDS_ACTION_1, 0), rules_custom.at (i).name);

						AppendMenu (submenu, MF_STRING, IDM_RULES_CUSTOM + i, buffer);

						if (rules_custom.at (i).is_enabled)
							CheckMenuItem (submenu, IDM_RULES_CUSTOM + UINT (i), MF_BYCOMMAND | MF_CHECKED);
					}
				}
			}

			app.LocaleMenu (menu, I18N (&app, IDS_HELP, 0), 4, TRUE);
			app.LocaleMenu (menu, I18N (&app, IDS_WEBSITE, 0), IDM_WEBSITE, FALSE);
			app.LocaleMenu (menu, I18N (&app, IDS_DONATE, 0), IDM_DONATE, FALSE);
			app.LocaleMenu (menu, I18N (&app, IDS_CHECKUPDATES, 0), IDM_CHECKUPDATES, FALSE);
			app.LocaleMenu (menu, I18N (&app, IDS_ABOUT, 0), IDM_ABOUT, FALSE);

			app.LocaleEnum ((HWND)GetSubMenu (menu, 2), 7, TRUE, IDM_LANGUAGE); // enum localizations

			SetDlgItemText (hwnd, IDC_START_BTN, I18N (&app, (config.is_filtersinstalled ? IDS_TRAY_STOP : IDS_TRAY_START), config.is_filtersinstalled ? L"IDS_TRAY_STOP" : L"IDS_TRAY_START"));
			SetDlgItemText (hwnd, IDC_SETTINGS_BTN, I18N (&app, IDS_SETTINGS, 0));
			SetDlgItemText (hwnd, IDC_EXIT_BTN, I18N (&app, IDS_EXIT, 0));

			_r_wnd_addstyle (hwnd, IDC_START_BTN, app.IsClassicUI () ? WS_EX_STATICEDGE : 0, WS_EX_STATICEDGE, GWL_EXSTYLE);
			_r_wnd_addstyle (hwnd, IDC_SETTINGS_BTN, app.IsClassicUI () ? WS_EX_STATICEDGE : 0, WS_EX_STATICEDGE, GWL_EXSTYLE);
			_r_wnd_addstyle (hwnd, IDC_EXIT_BTN, app.IsClassicUI () ? WS_EX_STATICEDGE : 0, WS_EX_STATICEDGE, GWL_EXSTYLE);

			_r_listview_resizeonecolumn (hwnd, IDC_LISTVIEW);

			_app_refreshstatus (hwnd, TRUE, TRUE); // refresh statusbar

			SendDlgItemMessage (hwnd, IDC_LISTVIEW, (LVM_FIRST + 84), 0, 0); // LVM_RESETEMPTYTEXT 

			break;
		}

		case _RM_UNINITIALIZE:
		{
			_app_loginit (FALSE); // disable dropped packets logging (win7 and above)
			app.TrayDestroy (UID);

			break;
		}
	}

	return FALSE;
}

INT_PTR CALLBACK EditorProc (HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam)
{
	static ITEM_RULE* ptr = nullptr;

	switch (msg)
	{
		case WM_INITDIALOG:
		{
			ptr = (ITEM_RULE*)lparam;

			// configure window
			_r_wnd_center (hwnd);

			// localize window
			SetWindowText (hwnd, I18N (&app, IDS_EDITOR, 0));

			SetDlgItemText (hwnd, IDC_NAME, I18N (&app, IDS_NAME, 0));
			SetDlgItemText (hwnd, IDC_RULES, I18N (&app, IDS_RULE, 0));
			SetDlgItemText (hwnd, IDC_DIRECTION, I18N (&app, IDS_DIRECTION, 0));
			SetDlgItemText (hwnd, IDC_PROTOCOL, I18N (&app, IDS_PROTOCOL, 0));
			SetDlgItemText (hwnd, IDC_IPVERSION, I18N (&app, IDS_IPVERSION, 0));
			SetDlgItemText (hwnd, IDC_ACTION, I18N (&app, IDS_ACTION, 0));
			SetDlgItemText (hwnd, IDC_RULES_LINKS, I18N (&app, IDS_RULES_LINKS, 0));
			SetDlgItemText (hwnd, IDC_ENABLED_CHK, I18N (&app, IDS_ENABLED_CHK, 0));
			SetDlgItemText (hwnd, IDC_APPLY, I18N (&app, IDS_APPLY, 0));
			SetDlgItemText (hwnd, IDC_CLOSE, I18N (&app, IDS_CLOSE, 0));

			_r_wnd_addstyle (hwnd, IDC_RULES_HELP, app.IsClassicUI () ? WS_EX_STATICEDGE : 0, WS_EX_STATICEDGE, GWL_EXSTYLE);
			_r_wnd_addstyle (hwnd, IDC_APPLY, app.IsClassicUI () ? WS_EX_STATICEDGE : 0, WS_EX_STATICEDGE, GWL_EXSTYLE);
			_r_wnd_addstyle (hwnd, IDC_CLOSE, app.IsClassicUI () ? WS_EX_STATICEDGE : 0, WS_EX_STATICEDGE, GWL_EXSTYLE);

			// set data
			SetDlgItemText (hwnd, IDC_NAME_EDIT, ptr->name);
			SetDlgItemText (hwnd, IDC_RULES_EDIT, ptr->rule);

			// dir
			SendDlgItemMessage (hwnd, IDC_DIRECTION_EDIT, CB_INSERTSTRING, 0, (LPARAM)I18N (&app, IDS_DIRECTION_1, 0).GetString ());
			SendDlgItemMessage (hwnd, IDC_DIRECTION_EDIT, CB_INSERTSTRING, 1, (LPARAM)I18N (&app, IDS_DIRECTION_2, 0).GetString ());
			SendDlgItemMessage (hwnd, IDC_DIRECTION_EDIT, CB_INSERTSTRING, 2, (LPARAM)I18N (&app, IDS_DIRECTION_3, 0).GetString ());

			SendDlgItemMessage (hwnd, IDC_DIRECTION_EDIT, CB_SETCURSEL, (WPARAM)ptr->dir, 0);

			// protocol
			SendDlgItemMessage (hwnd, IDC_PROTOCOL_EDIT, CB_INSERTSTRING, 0, (LPARAM)I18N (&app, IDS_ALL, 0).GetString ());
			SendDlgItemMessage (hwnd, IDC_PROTOCOL_EDIT, CB_SETCURSEL, 0, 0);

			for (size_t i = 0; i < protocols.size (); i++)
			{
				SendDlgItemMessage (hwnd, IDC_PROTOCOL_EDIT, CB_INSERTSTRING, i + 1, (LPARAM)protocols.at (i).t);
				SendDlgItemMessage (hwnd, IDC_PROTOCOL_EDIT, CB_SETITEMDATA, i + 1, (LPARAM)protocols.at (i).v);

				if (ptr->protocol == protocols.at (i).v)
					SendDlgItemMessage (hwnd, IDC_PROTOCOL_EDIT, CB_SETCURSEL, (WPARAM)i + 1, 0);
			}

			// af
			SendDlgItemMessage (hwnd, IDC_IPVERSION_EDIT, CB_INSERTSTRING, 0, (LPARAM)I18N (&app, IDS_ALL, 0).GetString ());
			SendDlgItemMessage (hwnd, IDC_IPVERSION_EDIT, CB_INSERTSTRING, 1, (LPARAM)L"IPv4");
			SendDlgItemMessage (hwnd, IDC_IPVERSION_EDIT, CB_INSERTSTRING, 2, (LPARAM)L"IPv6");

			if (ptr->version == AF_UNSPEC)
				SendDlgItemMessage (hwnd, IDC_IPVERSION_EDIT, CB_SETCURSEL, (WPARAM)0, 0);
			else if (ptr->version == AF_INET)
				SendDlgItemMessage (hwnd, IDC_IPVERSION_EDIT, CB_SETCURSEL, (WPARAM)1, 0);
			else if (ptr->version == AF_INET6)
				SendDlgItemMessage (hwnd, IDC_IPVERSION_EDIT, CB_SETCURSEL, (WPARAM)2, 0);

			// action
			SendDlgItemMessage (hwnd, IDC_ACTION_EDIT, CB_INSERTSTRING, 0, (LPARAM)I18N (&app, IDS_ACTION_1, 0).GetString ());
			SendDlgItemMessage (hwnd, IDC_ACTION_EDIT, CB_INSERTSTRING, 1, (LPARAM)I18N (&app, IDS_ACTION_2, 0).GetString ());

			SendDlgItemMessage (hwnd, IDC_ACTION_EDIT, CB_SETCURSEL, (WPARAM)ptr->is_block, 0);

			// state
			CheckDlgButton (hwnd, IDC_ENABLED_CHK, ptr->is_enabled ? BST_CHECKED : BST_UNCHECKED);

			// set limitation
			SendDlgItemMessage (hwnd, IDC_NAME_EDIT, EM_LIMITTEXT, _countof (ptr->name) - 1, 0);
			SendDlgItemMessage (hwnd, IDC_RULES_EDIT, EM_LIMITTEXT, _countof (ptr->rule) - 1, 0);

			_r_ctrl_enable (hwnd, IDC_APPLY, FALSE); // disable apply button

			break;
		}

		case WM_NOTIFY:
		{
			LPNMHDR nmlp = (LPNMHDR)lparam;

			switch (nmlp->code)
			{
				case NM_CLICK:
				case NM_RETURN:
				{
					if (nmlp->idFrom == IDC_RULES_LINKS)
					{
						PNMLINK nmlink = (PNMLINK)lparam;

						if (nmlink->item.szUrl)
							ShellExecute (hwnd, nullptr, nmlink->item.szUrl, nullptr, nullptr, SW_SHOWDEFAULT);
					}

					break;
				}
			}

			break;
		}

		case WM_COMMAND:
		{
			{
				const BOOL is_button = (GetWindowLongPtr (GetDlgItem (hwnd, LOWORD (wparam)), GWL_STYLE) & (BS_CHECKBOX | BS_RADIOBUTTON)) != 0;

				if ((HIWORD (wparam) == BN_CLICKED && is_button) || HIWORD (wparam) == EN_CHANGE || HIWORD (wparam) == CBN_SELENDOK)
				{
					const BOOL is_enable = (SendDlgItemMessage (hwnd, IDC_NAME_EDIT, WM_GETTEXTLENGTH, 0, 0) > 0) && (SendDlgItemMessage (hwnd, IDC_RULES_EDIT, WM_GETTEXTLENGTH, 0, 0) > 0);

					_r_ctrl_enable (hwnd, IDC_APPLY, is_enable); // enable apply button

					return FALSE;
				}
			}

			switch (LOWORD (wparam))
			{
				case IDOK: // process Enter key
				case IDC_APPLY:
				{
					StringCchCopy (ptr->name, _countof (ptr->name), _r_ctrl_gettext (hwnd, IDC_NAME_EDIT));
					StringCchCopy (ptr->rule, _countof (ptr->rule), _r_ctrl_gettext (hwnd, IDC_RULES_EDIT));

					// protocol
					{
						const UINT8 v = (UINT8)SendDlgItemMessage (hwnd, IDC_PROTOCOL_EDIT, CB_GETITEMDATA, SendDlgItemMessage (hwnd, IDC_PROTOCOL_EDIT, CB_GETCURSEL, 0, 0), 0);

						ptr->protocol = v;
					}

					// af
					{
						ADDRESS_FAMILY af = (ADDRESS_FAMILY)SendDlgItemMessage (hwnd, IDC_IPVERSION_EDIT, CB_GETCURSEL, 0, 0);

						if (af == 1)
							af = AF_INET;
						else if (af == 2)
							af = AF_INET6;

						ptr->version = af;
					}

					ptr->dir = (EnumDirection)SendDlgItemMessage (hwnd, IDC_DIRECTION_EDIT, CB_GETCURSEL, 0, 0);
					ptr->is_block = (BOOL)SendDlgItemMessage (hwnd, IDC_ACTION_EDIT, CB_GETCURSEL, 0, 0);
					ptr->is_enabled = IsDlgButtonChecked (hwnd, IDC_ENABLED_CHK) == BST_CHECKED;

					EndDialog (hwnd, 1);

					break;
				}

				case IDCANCEL: // process Esc key
				case IDC_CLOSE:
				{
					EndDialog (hwnd, 0);
					break;
				}

				case IDC_RULES_HELP:
				{
					_r_ctrl_showtip (hwnd, IDC_RULES_EDIT, nullptr, I18N (&app, IDS_RULES_HELP, 0), 0);
					break;
				}
			}

			break;
		}
	}

	return FALSE;
}

VOID SetIconsSize (HWND hwnd)
{
	HIMAGELIST h = nullptr;

	const BOOL is_large = app.ConfigGet (L"IsLargeIcons", FALSE).AsBool ();
	const BOOL is_iconshidden = app.ConfigGet (L"IsIconsHidden", FALSE).AsBool ();

	if (!is_iconshidden)
		SHGetImageList (is_large ? SHIL_LARGE : SHIL_SMALL, IID_IImageList, (LPVOID*)&h);

	SendDlgItemMessage (hwnd, IDC_LISTVIEW, LVM_SETIMAGELIST, LVSIL_SMALL, (LPARAM)h);
	SendDlgItemMessage (hwnd, IDC_LISTVIEW, LVM_SETIMAGELIST, LVSIL_NORMAL, (LPARAM)h);

	SendDlgItemMessage (hwnd, IDC_LISTVIEW, LVM_SCROLL, 0, GetScrollPos (GetDlgItem (hwnd, IDC_LISTVIEW), SB_VERT)); // scroll-resize-HACK!!!

	CheckMenuRadioItem (GetMenu (hwnd), IDM_ICONSSMALL, IDM_ICONSLARGE, (is_large ? IDM_ICONSLARGE : IDM_ICONSSMALL), MF_BYCOMMAND);
	CheckMenuItem (GetMenu (hwnd), IDM_ICONSISHIDDEN, MF_BYCOMMAND | (is_iconshidden ? MF_CHECKED : MF_UNCHECKED));

	_r_listview_resizeonecolumn (hwnd, IDC_LISTVIEW);
}

VOID ShowItem (HWND hwnd, UINT ctrl_id, size_t item)
{
	if (item == LAST_VALUE)
		return;

	ListView_SetItemState (GetDlgItem (hwnd, ctrl_id), -1, 0, LVIS_SELECTED); // deselect all
	ListView_SetItemState (GetDlgItem (hwnd, ctrl_id), item, LVIS_SELECTED, LVIS_SELECTED); // select item

	SendDlgItemMessage (hwnd, ctrl_id, LVM_ENSUREVISIBLE, item, TRUE); // ensure him visible
}

BOOL settings_callback (HWND hwnd, DWORD msg, LPVOID lpdata1, LPVOID lpdata2)
{
	PAPP_SETTINGS_PAGE const page = (PAPP_SETTINGS_PAGE)lpdata2;

	switch (msg)
	{
		case _RM_INITIALIZE:
		{
			// localize titles
			SetDlgItemText (hwnd, IDC_TITLE_1, I18N (&app, IDS_TITLE_1, 0));
			SetDlgItemText (hwnd, IDC_TITLE_2, I18N (&app, IDS_TITLE_2, 0));
			SetDlgItemText (hwnd, IDC_TITLE_3, I18N (&app, IDS_TITLE_3, 0));
			SetDlgItemText (hwnd, IDC_TITLE_4, I18N (&app, IDS_TITLE_4, 0));
			SetDlgItemText (hwnd, IDC_TITLE_5, I18N (&app, IDS_TITLE_5, 0));
			SetDlgItemText (hwnd, IDC_TITLE_6, I18N (&app, IDS_TITLE_6, 0));

			switch (page->dlg_id)
			{
				case IDD_SETTINGS_1:
				{
					// localize
					SetDlgItemText (hwnd, IDC_ALWAYSONTOP_CHK, I18N (&app, IDS_ALWAYSONTOP_CHK, 0));
					SetDlgItemText (hwnd, IDC_LOADONSTARTUP_CHK, I18N (&app, IDS_LOADONSTARTUP_CHK, 0));
					SetDlgItemText (hwnd, IDC_STARTMINIMIZED_CHK, I18N (&app, IDS_STARTMINIMIZED_CHK, 0));
					SetDlgItemText (hwnd, IDC_SKIPUACWARNING_CHK, I18N (&app, IDS_SKIPUACWARNING_CHK, 0));
					SetDlgItemText (hwnd, IDC_CHECKUPDATES_CHK, I18N (&app, IDS_CHECKUPDATES_CHK, 0));

					SetDlgItemText (hwnd, IDC_LANGUAGE_HINT, I18N (&app, IDS_LANGUAGE_HINT, 0));

					if (!app.IsAdmin () || !app.IsVistaOrLater ())
						_r_ctrl_enable (hwnd, IDC_SKIPUACWARNING_CHK, FALSE);

					CheckDlgButton (hwnd, IDC_ALWAYSONTOP_CHK, app.ConfigGet (L"AlwaysOnTop", FALSE).AsBool () ? BST_CHECKED : BST_UNCHECKED);
					CheckDlgButton (hwnd, IDC_LOADONSTARTUP_CHK, app.AutorunIsPresent () ? BST_CHECKED : BST_UNCHECKED);
					CheckDlgButton (hwnd, IDC_STARTMINIMIZED_CHK, app.ConfigGet (L"StartMinimized", TRUE).AsBool () ? BST_CHECKED : BST_UNCHECKED);
#ifdef _APP_HAVE_SKIPUAC
					if (app.IsAdmin ())
						CheckDlgButton (hwnd, IDC_SKIPUACWARNING_CHK, app.SkipUacIsPresent (FALSE) ? BST_CHECKED : BST_UNCHECKED);
#endif // _APP_HAVE_SKIPUAC
					CheckDlgButton (hwnd, IDC_CHECKUPDATES_CHK, app.ConfigGet (L"CheckUpdates", TRUE).AsBool () ? BST_CHECKED : BST_UNCHECKED);

					app.LocaleEnum (hwnd, IDC_LANGUAGE, FALSE, 0);

					SetWindowLongPtr (hwnd, GWLP_USERDATA, (LONG_PTR)SendDlgItemMessage (hwnd, IDC_LANGUAGE, CB_GETCURSEL, 0, 0)); // check on save

					break;
				}

				case IDD_SETTINGS_2:
				{
					// localize
					SetDlgItemText (hwnd, IDC_USEBLOCKLIST_CHK, I18N (&app, IDS_USEBLOCKLIST_CHK, 0));
					SetDlgItemText (hwnd, IDC_USEBLOCKLIST_HINT, I18N (&app, IDS_USEBLOCKLIST_HINT, 0));

					SetDlgItemText (hwnd, IDC_USEUPDATECHECKING_CHK, _r_fmt (I18N (&app, IDS_USEUPDATECHECKING_CHK, 0), APP_NAME));
					SetDlgItemText (hwnd, IDC_USEUPDATECHECKING_HINT, _r_fmt (I18N (&app, IDS_USEUPDATECHECKING_HINT, 0), APP_NAME));

					SetDlgItemText (hwnd, IDC_INSTALLBOOTTIMEFILTERS_CHK, I18N (&app, IDS_INSTALLBOOTTIMEFILTERS_CHK, 0));
					SetDlgItemText (hwnd, IDC_INSTALLBOOTTIMEFILTERS_HINT, I18N (&app, IDS_INSTALLBOOTTIMEFILTERS_HINT, 0));

					SetDlgItemText (hwnd, IDC_RULE_OUTBOUND_ICMP, I18N (&app, IDS_RULE_OUTBOUND_ICMP, 0));
					SetDlgItemText (hwnd, IDC_RULE_INBOUND_ICMP, I18N (&app, IDS_RULE_INBOUND_ICMP, 0));
					SetDlgItemText (hwnd, IDC_RULE_INBOUND, I18N (&app, IDS_RULE_INBOUND, 0));

					CheckDlgButton (hwnd, IDC_USEBLOCKLIST_CHK, app.ConfigGet (L"UseBlocklist2", FALSE).AsBool () ? BST_CHECKED : BST_UNCHECKED);
					CheckDlgButton (hwnd, IDC_INSTALLBOOTTIMEFILTERS_CHK, app.ConfigGet (L"InstallBoottimeFilters", FALSE).AsBool () ? BST_CHECKED : BST_UNCHECKED);
					CheckDlgButton (hwnd, IDC_USEUPDATECHECKING_CHK, app.ConfigGet (L"AllowInternetAccess", TRUE).AsBool () ? BST_CHECKED : BST_UNCHECKED);

					CheckDlgButton (hwnd, IDC_RULE_OUTBOUND_ICMP, app.ConfigGet (L"AllowOutboundIcmp", TRUE).AsBool () ? BST_CHECKED : BST_UNCHECKED);
					CheckDlgButton (hwnd, IDC_RULE_INBOUND_ICMP, app.ConfigGet (L"AllowInboundIcmp", FALSE).AsBool () ? BST_CHECKED : BST_UNCHECKED);
					CheckDlgButton (hwnd, IDC_RULE_INBOUND, app.ConfigGet (L"AllowInboundConnections", FALSE).AsBool () ? BST_CHECKED : BST_UNCHECKED);

					break;
				}

				case IDD_SETTINGS_3:
				{
					// localize
					SetDlgItemText (hwnd, IDC_CONFIRMEXIT_CHK, I18N (&app, IDS_CONFIRMEXIT_CHK, 0));
					SetDlgItemText (hwnd, IDC_CONFIRMDELETE_CHK, I18N (&app, IDS_CONFIRMDELETE_CHK, 0));
					SetDlgItemText (hwnd, IDC_CONFIRMLOGCLEAR_CHK, I18N (&app, IDS_CONFIRMLOGCLEAR_CHK, 0));
					SetDlgItemText (hwnd, IDC_CONFIRMMODECHANGE_CHK, I18N (&app, IDS_CONFIRMMODECHANGE_CHK, 0));

					SetDlgItemText (hwnd, IDC_COLORS_HELP, I18N (&app, IDS_COLORS_HELP, 0));

					CheckDlgButton (hwnd, IDC_CONFIRMEXIT_CHK, app.ConfigGet (L"ConfirmExit", TRUE).AsBool () ? BST_CHECKED : BST_UNCHECKED);
					CheckDlgButton (hwnd, IDC_CONFIRMDELETE_CHK, app.ConfigGet (L"ConfirmDelete", TRUE).AsBool () ? BST_CHECKED : BST_UNCHECKED);
					CheckDlgButton (hwnd, IDC_CONFIRMLOGCLEAR_CHK, app.ConfigGet (L"ConfirmLogClear", TRUE).AsBool () ? BST_CHECKED : BST_UNCHECKED);
					CheckDlgButton (hwnd, IDC_CONFIRMMODECHANGE_CHK, app.ConfigGet (L"ConfirmModeChange", TRUE).AsBool () ? BST_CHECKED : BST_UNCHECKED);

					// configure listview
					_r_listview_setstyle (hwnd, IDC_COLORS, LVS_EX_DOUBLEBUFFER | LVS_EX_FULLROWSELECT | LVS_EX_INFOTIP | LVS_EX_LABELTIP | LVS_EX_CHECKBOXES);

					_r_listview_deleteallitems (hwnd, IDC_COLORS);
					_r_listview_deleteallcolumns (hwnd, IDC_COLORS);

					_r_listview_addcolumn (hwnd, IDC_COLORS, nullptr, 95, 0, LVCFMT_LEFT);

					for (size_t i = 0; i < colors.size (); i++)
					{
						colors.at (i).clr = app.ConfigGet (colors.at (i).config_color, colors.at (i).default_clr).AsUlong ();

						_r_listview_additem (hwnd, IDC_COLORS, I18N (&app, colors.at (i).locale_id, colors.at (i).locale_sid), LAST_VALUE, 0, LAST_VALUE, LAST_VALUE, i);

						if (app.ConfigGet (colors.at (i).config, colors.at (i).is_enabled).AsBool ())
							_r_listview_setcheckstate (hwnd, IDC_COLORS, LAST_VALUE, TRUE);
					}

					break;
				}

				case IDD_SETTINGS_4:
				{
					// localize
					SetDlgItemText (hwnd, IDC_ENABLELOG_CHK, I18N (&app, IDS_ENABLELOG_CHK, 0));

					SetDlgItemText (hwnd, IDC_ENABLENOTIFICATIONS_CHK, I18N (&app, IDS_ENABLENOTIFICATIONS_CHK, 0));
					SetDlgItemText (hwnd, IDC_NOTIFICATIONSILENT_CHK, I18N (&app, IDS_NOTIFICATIONSILENT_CHK, 0));
					SetDlgItemText (hwnd, IDC_NOTIFICATIONTIMEOUT_HINT, I18N (&app, IDS_NOTIFICATIONTIMEOUT_HINT, 0));

					SetDlgItemText (hwnd, IDC_ENABLEAPPSCOLLECTOR_CHK, I18N (&app, IDS_ENABLEAPPSCOLLECTOR_CHK, 0));

					if (!app.IsAdmin () || !_r_sys_validversion (6, 1))
					{
						_r_ctrl_enable (hwnd, IDC_ENABLELOG_CHK, FALSE);
						_r_ctrl_enable (hwnd, IDC_ENABLENOTIFICATIONS_CHK, FALSE);
						_r_ctrl_enable (hwnd, IDC_ENABLEAPPSCOLLECTOR_CHK, FALSE);
					}

					CheckDlgButton (hwnd, IDC_ENABLELOG_CHK, app.ConfigGet (L"IsLogEnabled", FALSE).AsBool () ? BST_CHECKED : BST_UNCHECKED);

					SetDlgItemText (hwnd, IDC_LOGPATH, app.ConfigGet (L"LogPath", PATH_LOG));

					CheckDlgButton (hwnd, IDC_ENABLENOTIFICATIONS_CHK, app.ConfigGet (L"IsNotificationsEnabled", TRUE).AsBool () ? BST_CHECKED : BST_UNCHECKED);
					CheckDlgButton (hwnd, IDC_NOTIFICATIONSILENT_CHK, app.ConfigGet (L"IsNotificationsSilent", FALSE).AsBool () ? BST_CHECKED : BST_UNCHECKED);

					SendDlgItemMessage (hwnd, IDC_NOTIFICATIONTIMEOUT, UDM_SETRANGE32, 1, 86400);
					SendDlgItemMessage (hwnd, IDC_NOTIFICATIONTIMEOUT, UDM_SETPOS32, 0, app.ConfigGet (L"NotificationsTimeout", 10).AsUint ());

					CheckDlgButton (hwnd, IDC_ENABLEAPPSCOLLECTOR_CHK, app.ConfigGet (L"IsAppsCollectorEnabled", TRUE).AsBool () ? BST_CHECKED : BST_UNCHECKED);

					_r_wnd_addstyle (hwnd, IDC_LOGPATH_BTN, app.IsClassicUI () ? WS_EX_STATICEDGE : 0, WS_EX_STATICEDGE, GWL_EXSTYLE);

					SendMessage (hwnd, WM_COMMAND, MAKEWPARAM (IDC_ENABLELOG_CHK, 0), 0);
					SendMessage (hwnd, WM_COMMAND, MAKEWPARAM (IDC_ENABLENOTIFICATIONS_CHK, 0), 0);

					break;
				}

				case IDD_SETTINGS_5:
				case IDD_SETTINGS_6:
				case IDD_SETTINGS_7:
				{
					// configure listview
					_r_listview_setstyle (hwnd, IDC_EDITOR, LVS_EX_DOUBLEBUFFER | LVS_EX_FULLROWSELECT | LVS_EX_INFOTIP | LVS_EX_LABELTIP | LVS_EX_CHECKBOXES);

					const size_t item = (size_t)SendDlgItemMessage (hwnd, IDC_EDITOR, LVM_GETNEXTITEM, (WPARAM)-1, LVNI_SELECTED);

					SendDlgItemMessage (hwnd, IDC_EDITOR, LVM_SETIMAGELIST, LVSIL_SMALL, (LPARAM)config.himg);
					SendDlgItemMessage (hwnd, IDC_EDITOR, LVM_SETIMAGELIST, LVSIL_NORMAL, (LPARAM)config.himg);

					_r_listview_deleteallitems (hwnd, IDC_EDITOR);
					_r_listview_deleteallcolumns (hwnd, IDC_EDITOR);

					_r_listview_addcolumn (hwnd, IDC_EDITOR, I18N (&app, IDS_NAME, 0), 40, 1, LVCFMT_LEFT);
					_r_listview_addcolumn (hwnd, IDC_EDITOR, I18N (&app, IDS_DIRECTION, 0), 22, 2, LVCFMT_LEFT);
					_r_listview_addcolumn (hwnd, IDC_EDITOR, I18N (&app, IDS_PROTOCOL, 0), 20, 3, LVCFMT_LEFT);
					_r_listview_addcolumn (hwnd, IDC_EDITOR, I18N (&app, IDS_IPVERSION, 0), 13, 4, LVCFMT_LEFT);

					std::vector<ITEM_RULE> const* ptr = nullptr;

					if (page->dlg_id == IDD_SETTINGS_5)
					{
						ptr = &rules_system;
					}
					else if (page->dlg_id == IDD_SETTINGS_6)
					{
						ptr = &rules_custom;
					}
					else if (page->dlg_id == IDD_SETTINGS_7)
					{
						ptr = &rules_blocklist;
					}

					if (ptr)
					{
						for (size_t i = 0; i < ptr->size (); i++)
						{
							rstring dir = I18N (&app, IDS_DIRECTION_1 + ptr->at (i).dir, _r_fmt (L"IDS_DIRECTION_%d", ptr->at (i).dir + 1));
							rstring protocol = I18N (&app, IDS_ALL, 0);
							rstring af = protocol;

							// protocol
							for (size_t j = 0; j < protocols.size (); j++)
							{
								if (ptr->at (i).protocol == protocols.at (j).v)
									protocol = protocols.at (j).t;
							}

							// af
							if (ptr->at (i).version == AF_INET)
								af = L"IPv4";
							if (ptr->at (i).version == AF_INET6)
								af = L"IPv6";

							_r_listview_additem (hwnd, IDC_EDITOR, ptr->at (i).name, i, 0, ptr->at (i).is_block ? 1 : 0, LAST_VALUE, i);
							_r_listview_additem (hwnd, IDC_EDITOR, dir, i, 1);
							_r_listview_additem (hwnd, IDC_EDITOR, protocol, i, 2);
							_r_listview_additem (hwnd, IDC_EDITOR, af, i, 3);

							_r_listview_setcheckstate (hwnd, IDC_EDITOR, i, ptr->at (i).is_enabled);
						}
					}

					ShowItem (hwnd, IDC_EDITOR, item);

					break;
				}
			}

			break;
		}

		case _RM_MESSAGE:
		{
			LPMSG pmsg = (LPMSG)lpdata1;

			switch (pmsg->message)
			{
				case WM_NOTIFY:
				{
					LPNMHDR nmlp = (LPNMHDR)pmsg->lParam;

					switch (nmlp->code)
					{
						case LVN_GETINFOTIP:
						{
							if (nmlp->idFrom != IDC_EDITOR)
								break;

							LPNMLVGETINFOTIP lpnmlv = (LPNMLVGETINFOTIP)pmsg->lParam;
							ITEM_RULE const* ptr = nullptr;

							const size_t idx = (size_t)lpnmlv->iItem;

							if (page->dlg_id == IDD_SETTINGS_6)
								ptr = &rules_custom.at (idx);
							else if (page->dlg_id == IDD_SETTINGS_7)
								ptr = &rules_blocklist.at (idx);
							else if (page->dlg_id == IDD_SETTINGS_5)
								ptr = &rules_system.at (idx);

							if (ptr)
							{
								StringCchPrintf (lpnmlv->pszText, lpnmlv->cchTextMax, L"%s\r\n%s", ptr->name, !ptr->rule[0] ? NA_TEXT : ptr->rule);
							}

							break;
						}

						case NM_CUSTOMDRAW:
						{
							LONG result = CDRF_DODEFAULT;
							LPNMLVCUSTOMDRAW lpnmlv = (LPNMLVCUSTOMDRAW)pmsg->lParam;

							if (nmlp->idFrom != IDC_COLORS && nmlp->idFrom != IDC_EDITOR)
								break;

							switch (lpnmlv->nmcd.dwDrawStage)
							{
								case CDDS_PREPAINT:
								{
									result = CDRF_NOTIFYITEMDRAW;
									break;
								}

								case CDDS_ITEMPREPAINT:
								{
									if (nmlp->idFrom == IDC_COLORS)
									{
										lpnmlv->clrTextBk = colors.at (lpnmlv->nmcd.lItemlParam).clr;
										_r_wnd_fillrect (lpnmlv->nmcd.hdc, &lpnmlv->nmcd.rc, lpnmlv->clrTextBk);

										result = CDRF_NEWFONT;
									}
									else if (nmlp->idFrom == IDC_EDITOR)
									{
										if (lpnmlv->nmcd.dwItemSpec % 2)
										{
											lpnmlv->clrTextBk = _R_COLOR_SHADE (GetSysColor (COLOR_WINDOW), 95.0);
											_r_wnd_fillrect (lpnmlv->nmcd.hdc, &lpnmlv->nmcd.rc, lpnmlv->clrTextBk);

											result = CDRF_NEWFONT;
										}
									}

									break;
								}
							}

							SetWindowLongPtr (hwnd, DWLP_MSGRESULT, result);
							return TRUE;
						}

						case NM_DBLCLK:
						{
							LPNMITEMACTIVATE lpnmlv = (LPNMITEMACTIVATE)pmsg->lParam;

							if (lpnmlv->iItem != -1)
							{
								if (nmlp->idFrom == IDC_COLORS)
								{
									size_t idx = _r_listview_getlparam (hwnd, IDC_COLORS, lpnmlv->iItem);

									CHOOSECOLOR cc = {0};
									COLORREF cust[16] = {LISTVIEW_COLOR_CUSTOM, LISTVIEW_COLOR_SYSTEM, LISTVIEW_COLOR_NETWORK, LISTVIEW_COLOR_INVALID, LISTVIEW_COLOR_SILENT};

									cc.lStructSize = sizeof (cc);
									cc.Flags = CC_RGBINIT | CC_FULLOPEN;
									cc.hwndOwner = hwnd;
									cc.lpCustColors = cust;
									cc.rgbResult = colors.at (idx).clr;

									if (ChooseColor (&cc))
									{
										colors.at (idx).clr = cc.rgbResult;

										_r_ctrl_enable (GetParent (hwnd), IDC_APPLY, TRUE); // enable apply button (required!)
									}
								}
								else if (nmlp->idFrom == IDC_EDITOR)
								{
									SendMessage (hwnd, WM_COMMAND, MAKELPARAM (IDM_EDIT, 0), 0);
								}
							}

							break;
						}
					}

					break;
				}

				case WM_CONTEXTMENU:
				{
					UINT ctrl_id = GetDlgCtrlID ((HWND)pmsg->wParam);

					if (ctrl_id == IDC_EDITOR)
					{
						HMENU menu = LoadMenu (nullptr, MAKEINTRESOURCE (IDM_EDITOR)), submenu = GetSubMenu (menu, 0);

						// localize
						app.LocaleMenu (submenu, I18N (&app, IDS_ADD, 0), IDM_ADD, FALSE);
						app.LocaleMenu (submenu, I18N (&app, IDS_EDIT2, 0), IDM_EDIT, FALSE);
						app.LocaleMenu (submenu, I18N (&app, IDS_DELETE, 0), IDM_DELETE, FALSE);
						app.LocaleMenu (submenu, I18N (&app, IDS_CHECK, 0), IDM_CHECK, FALSE);
						app.LocaleMenu (submenu, I18N (&app, IDS_UNCHECK, 0), IDM_UNCHECK, FALSE);

						if (!SendDlgItemMessage (hwnd, ctrl_id, LVM_GETSELECTEDCOUNT, 0, 0))
						{
							EnableMenuItem (submenu, IDM_EDIT, MF_BYCOMMAND | MF_DISABLED);
							EnableMenuItem (submenu, IDM_DELETE, MF_BYCOMMAND | MF_DISABLED);
							EnableMenuItem (submenu, IDM_CHECK, MF_BYCOMMAND | MF_DISABLED);
							EnableMenuItem (submenu, IDM_UNCHECK, MF_BYCOMMAND | MF_DISABLED);
						}

						if (page->dlg_id != IDD_SETTINGS_6)
						{
							DeleteMenu (submenu, IDM_ADD, MF_BYCOMMAND);
							DeleteMenu (submenu, IDM_EDIT, MF_BYCOMMAND);
							DeleteMenu (submenu, IDM_DELETE, MF_BYCOMMAND);
							DeleteMenu (submenu, 0, MF_BYPOSITION);
						}

						POINT pt = {0};
						GetCursorPos (&pt);

						TrackPopupMenuEx (submenu, TPM_RIGHTBUTTON | TPM_LEFTBUTTON, pt.x, pt.y, hwnd, nullptr);

						DestroyMenu (menu);
						DestroyMenu (submenu);
					}
				}

				case WM_COMMAND:
				{
					switch (LOWORD (pmsg->wParam))
					{
						case IDC_ENABLELOG_CHK:
						{
							const UINT ctrl = LOWORD (pmsg->wParam);

							const BOOL is_enabled = IsWindowEnabled (GetDlgItem (hwnd, ctrl)) && (IsDlgButtonChecked (hwnd, ctrl) == BST_CHECKED);

							_r_ctrl_enable (hwnd, IDC_LOGPATH, is_enabled); // input
							_r_ctrl_enable (hwnd, IDC_LOGPATH_BTN, is_enabled); // button

							break;
						}

						case IDC_ENABLENOTIFICATIONS_CHK:
						{
							const UINT ctrl = LOWORD (pmsg->wParam);

							const BOOL is_enabled = IsWindowEnabled (GetDlgItem (hwnd, ctrl)) && (IsDlgButtonChecked (hwnd, ctrl) == BST_CHECKED);

							_r_ctrl_enable (hwnd, IDC_NOTIFICATIONSILENT_CHK, is_enabled);

							EnableWindow ((HWND)SendDlgItemMessage (hwnd, IDC_NOTIFICATIONTIMEOUT, UDM_GETBUDDY, 0, 0), is_enabled);

							break;
						}

						case IDC_LOGPATH_BTN:
						{
							OPENFILENAME ofn = {0};

							WCHAR path[512] = {0};
							GetDlgItemText (hwnd, IDC_LOGPATH, path, _countof (path));
							StringCchCopy (path, _countof (path), _r_path_expand (path));

							ofn.lStructSize = sizeof (ofn);
							ofn.hwndOwner = hwnd;
							ofn.lpstrFile = path;
							ofn.nMaxFile = _countof (path);
							ofn.lpstrFileTitle = APP_NAME_SHORT;
							ofn.nMaxFile = _countof (path);
							ofn.lpstrFilter = L"*.log\0*.log\0\0";
							ofn.lpstrDefExt = L"log";
							ofn.Flags = OFN_EXPLORER | OFN_ENABLESIZING | OFN_HIDEREADONLY | OFN_OVERWRITEPROMPT;

							if (GetSaveFileName (&ofn))
								SetDlgItemText (hwnd, IDC_LOGPATH, _r_path_unexpand (path));

							break;
						}

						case IDM_ADD:
						{
							if (page->dlg_id != IDD_SETTINGS_6)
								break;

							ITEM_RULE* ptr = new ITEM_RULE;

							if (ptr)
							{
								SecureZeroMemory (ptr, sizeof (ITEM_RULE));

								ptr->is_enabled = TRUE; // enabled by default

								if (DialogBoxParam (nullptr, MAKEINTRESOURCE (IDD_EDITOR), hwnd, &EditorProc, (LPARAM)ptr))
								{
									rules_custom.push_back (*ptr);
									settings_callback (page->hwnd, _RM_INITIALIZE, nullptr, page); // reinititalize page
								}

								delete ptr;
							}

							break;
						}

						case IDM_EDIT:
						{
							if (page->dlg_id != IDD_SETTINGS_6)
								break;

							const size_t item = (size_t)SendDlgItemMessage (hwnd, IDC_EDITOR, LVM_GETNEXTITEM, (WPARAM)-1, LVNI_SELECTED);

							if (item == LAST_VALUE)
								break;

							ITEM_RULE* ptr = &rules_custom.at (item);

							if (DialogBoxParam (nullptr, MAKEINTRESOURCE (IDD_EDITOR), hwnd, &EditorProc, (LPARAM)ptr))
							{
								settings_callback (page->hwnd, _RM_INITIALIZE, nullptr, page); // re-inititalize page
							}

							break;
						}

						case IDM_DELETE:
						{
							if (page->dlg_id != IDD_SETTINGS_6)
								break;

							if (app.ConfigGet (L"ConfirmDelete", TRUE).AsBool () && _r_msg (hwnd, MB_YESNO | MB_ICONQUESTION, APP_NAME, nullptr, I18N (&app, IDS_QUESTION, 0)) != IDYES)
								break;

							const size_t count = _r_listview_getitemcount (hwnd, IDC_EDITOR) - 1;

							for (size_t i = count; i != LAST_VALUE; i--)
							{
								if (ListView_GetItemState (GetDlgItem (hwnd, IDC_EDITOR), i, LVNI_SELECTED))
								{
									rules_custom.erase (rules_custom.begin () + i);

									SendDlgItemMessage (hwnd, IDC_EDITOR, LVM_DELETEITEM, i, 0);
								}
							}

							SendDlgItemMessage (hwnd, IDC_EDITOR, LVM_REDRAWITEMS, 0, count); // redraw (required!)

							break;
						}

						case IDM_UNCHECK:
						case IDM_CHECK:
						{
							INT item = -1;

							while ((item = (INT)SendDlgItemMessage (hwnd, IDC_EDITOR, LVM_GETNEXTITEM, item, LVNI_SELECTED)) != -1)
							{
								if (LOWORD (pmsg->wParam) == IDM_CHECK || LOWORD (pmsg->wParam) == IDM_UNCHECK)
								{
									_r_listview_setcheckstate (hwnd, IDC_EDITOR, item, LOWORD (pmsg->wParam) == IDM_CHECK ? TRUE : FALSE);
								}
							}

							break;
						}
					}

					break;
				}
			}

			break;
		}

		case _RM_SAVE:
		{
			switch (page->dlg_id)
			{
				case IDD_SETTINGS_1:
				{
					app.ConfigSet (L"AlwaysOnTop", DWORD ((IsDlgButtonChecked (hwnd, IDC_ALWAYSONTOP_CHK) == BST_CHECKED) ? TRUE : FALSE));
					app.AutorunCreate (IsDlgButtonChecked (hwnd, IDC_LOADONSTARTUP_CHK) == BST_UNCHECKED);
					app.ConfigSet (L"StartMinimized", DWORD ((IsDlgButtonChecked (hwnd, IDC_STARTMINIMIZED_CHK) == BST_CHECKED) ? TRUE : FALSE));

#ifdef _APP_HAVE_SKIPUAC
					if (!_r_sys_uacstate ())
						app.SkipUacCreate (IsDlgButtonChecked (hwnd, IDC_SKIPUACWARNING_CHK) == BST_UNCHECKED);
#endif // _APP_HAVE_SKIPUAC

					app.ConfigSet (L"CheckUpdates", ((IsDlgButtonChecked (hwnd, IDC_CHECKUPDATES_CHK) == BST_CHECKED) ? TRUE : FALSE));

					// set language
					rstring buffer;

					if (SendDlgItemMessage (hwnd, IDC_LANGUAGE, CB_GETCURSEL, 0, 0) >= 1)
						buffer = _r_ctrl_gettext (hwnd, IDC_LANGUAGE);

					app.ConfigSet (L"Language", buffer);

					if (GetWindowLongPtr (hwnd, GWLP_USERDATA) != (INT)SendDlgItemMessage (hwnd, IDC_LANGUAGE, CB_GETCURSEL, 0, 0))
						return TRUE; // for restart

					break;
				}

				case IDD_SETTINGS_2:
				{
					app.ConfigSet (L"UseBlocklist2", DWORD ((IsDlgButtonChecked (hwnd, IDC_USEBLOCKLIST_CHK) == BST_CHECKED) ? TRUE : FALSE));
					app.ConfigSet (L"AllowInternetAccess", DWORD ((IsDlgButtonChecked (hwnd, IDC_USEUPDATECHECKING_CHK) == BST_CHECKED) ? TRUE : FALSE));
					app.ConfigSet (L"InstallBoottimeFilters", DWORD ((IsDlgButtonChecked (hwnd, IDC_INSTALLBOOTTIMEFILTERS_CHK) == BST_CHECKED) ? TRUE : FALSE));

					app.ConfigSet (L"AllowOutboundIcmp", DWORD ((IsDlgButtonChecked (hwnd, IDC_RULE_OUTBOUND_ICMP) == BST_CHECKED) ? TRUE : FALSE));
					app.ConfigSet (L"AllowInboundIcmp", DWORD ((IsDlgButtonChecked (hwnd, IDC_RULE_INBOUND_ICMP) == BST_CHECKED) ? TRUE : FALSE));
					app.ConfigSet (L"AllowInboundConnections", DWORD ((IsDlgButtonChecked (hwnd, IDC_RULE_INBOUND) == BST_CHECKED) ? TRUE : FALSE));

					break;
				}

				case IDD_SETTINGS_3:
				{
					app.ConfigSet (L"ConfirmExit", DWORD ((IsDlgButtonChecked (hwnd, IDC_CONFIRMEXIT_CHK) == BST_CHECKED) ? TRUE : FALSE));
					app.ConfigSet (L"ConfirmDelete", DWORD ((IsDlgButtonChecked (hwnd, IDC_CONFIRMDELETE_CHK) == BST_CHECKED) ? TRUE : FALSE));
					app.ConfigSet (L"ConfirmLogClear", DWORD ((IsDlgButtonChecked (hwnd, IDC_CONFIRMLOGCLEAR_CHK) == BST_CHECKED) ? TRUE : FALSE));
					app.ConfigSet (L"ConfirmModeChange", DWORD ((IsDlgButtonChecked (hwnd, IDC_CONFIRMMODECHANGE_CHK) == BST_CHECKED) ? TRUE : FALSE));

					for (size_t i = 0; i < colors.size (); i++)
					{
						app.ConfigSet (colors.at (i).config, _r_listview_getcheckstate (hwnd, IDC_COLORS, i));
						app.ConfigSet (colors.at (i).config_color, colors.at (i).clr);
					}

					break;
				}

				case IDD_SETTINGS_4:
				{
					app.ConfigSet (L"IsLogEnabled", DWORD ((IsDlgButtonChecked (hwnd, IDC_ENABLELOG_CHK) == BST_CHECKED) ? TRUE : FALSE));
					app.ConfigSet (L"LogPath", _r_ctrl_gettext (hwnd, IDC_LOGPATH));

					app.ConfigSet (L"IsNotificationsEnabled", DWORD ((IsDlgButtonChecked (hwnd, IDC_ENABLENOTIFICATIONS_CHK) == BST_CHECKED) ? TRUE : FALSE));
					app.ConfigSet (L"IsNotificationsSilent", DWORD ((IsDlgButtonChecked (hwnd, IDC_NOTIFICATIONSILENT_CHK) == BST_CHECKED) ? TRUE : FALSE));
					app.ConfigSet (L"NotificationsTimeout", (DWORD)SendDlgItemMessage (hwnd, IDC_NOTIFICATIONTIMEOUT, UDM_GETPOS32, 0, 0));

					app.ConfigSet (L"IsAppsCollectorEnabled", DWORD ((IsDlgButtonChecked (hwnd, IDC_ENABLEAPPSCOLLECTOR_CHK) == BST_CHECKED) ? TRUE : FALSE));

					break;
				}

				case IDD_SETTINGS_7:
				{
					for (size_t i = 0; i < _r_listview_getitemcount (hwnd, IDC_EDITOR); i++)
					{
						rules_blocklist.at (i).is_enabled = _r_listview_getcheckstate (hwnd, IDC_EDITOR, i);
						app.ConfigSet (rules_blocklist.at (i).name, rules_blocklist.at (i).is_enabled, SECTION_BLOCKLIST);
					}

					break;
				}

				case IDD_SETTINGS_5:
				{
					for (size_t i = 0; i < _r_listview_getitemcount (hwnd, IDC_EDITOR); i++)
					{
						rules_system.at (i).is_enabled = _r_listview_getcheckstate (hwnd, IDC_EDITOR, i);
						app.ConfigSet (rules_system.at (i).name, _r_listview_getcheckstate (hwnd, IDC_EDITOR, i), SECTION_SYSTEM);
					}

					break;
				}

				case IDD_SETTINGS_6:
				{
					for (size_t i = 0; i < _r_listview_getitemcount (hwnd, IDC_EDITOR); i++)
					{
						rules_custom.at (i).is_enabled = _r_listview_getcheckstate (hwnd, IDC_EDITOR, i);
					}

					_app_profilesave (app.GetHWND ()); // save profile

					break;
				}
			}

			break;
		}

		case _RM_UNINITIALIZE:
		{
			_app_profileload (app.GetHWND ());
			break;
		}
	}

	return FALSE;
}

VOID ResizeWindow (HWND hwnd, INT width, INT height)
{
	RECT rc = {0};

	GetClientRect (GetDlgItem (hwnd, IDC_EXIT_BTN), &rc);
	INT button_width = rc.right;

	INT button_top = height - config.statusbar_height - app.GetDPI (1 + 34);

	SetWindowPos (GetDlgItem (hwnd, IDC_LISTVIEW), nullptr, 0, 0, width, height - config.statusbar_height - app.GetDPI (1 + 46), SWP_NOZORDER | SWP_NOACTIVATE);

	SetWindowPos (GetDlgItem (hwnd, IDC_START_BTN), nullptr, app.GetDPI (10), button_top, 0, 0, SWP_NOZORDER | SWP_NOACTIVATE | SWP_NOSIZE);
	SetWindowPos (GetDlgItem (hwnd, IDC_SETTINGS_BTN), nullptr, width - app.GetDPI (10) - button_width - button_width - app.GetDPI (6), button_top, 0, 0, SWP_NOZORDER | SWP_NOACTIVATE | SWP_NOSIZE);
	SetWindowPos (GetDlgItem (hwnd, IDC_EXIT_BTN), nullptr, width - app.GetDPI (10) - button_width, button_top, 0, 0, SWP_NOZORDER | SWP_NOACTIVATE | SWP_NOSIZE);

	// resize statusbar parts
	INT parts[] = {_R_PERCENT_VAL (45, width), -1};
	SendDlgItemMessage (hwnd, IDC_STATUSBAR, SB_SETPARTS, 2, (LPARAM)parts);

	// resize column width
	_r_listview_resizeonecolumn (hwnd, IDC_LISTVIEW);

	// resize statusbar
	SendDlgItemMessage (hwnd, IDC_STATUSBAR, WM_SIZE, 0, 0);
}

VOID _wfp_initialize ()
{
	FWPM_SESSION session = {0};

	session.displayData.name = APP_NAME;
	session.displayData.description = APP_NAME;

	DWORD result = FwpmEngineOpen (nullptr, RPC_C_AUTHN_WINNT, nullptr, &session, &config.hengine);

	if (result != ERROR_SUCCESS)
	{
		WDBG (L"FwpmEngineOpen failed. Return value: 0x%.8lx.", result);
	}
	else
	{
		// net events subscribe (win7 and above)
		if (app.IsAdmin ())
		{
			if (_r_sys_validversion (6, 1))
			{
				FWP_VALUE val;
				SecureZeroMemory (&val, sizeof (val));

				val.type = FWP_UINT32;
				val.uint32 = 1;

				result = FwpmEngineSetOption (config.hengine, FWPM_ENGINE_COLLECT_NET_EVENTS, &val);

				if (result != ERROR_SUCCESS)
				{
					WDBG (L"FwpmEngineSetOption failed. Return value: 0x%.8lx.", result);
				}
				else
				{
					if (!config.hevent)
					{
						FWPMNES0 _FwpmNetEventSubscribe0 = (FWPMNES0)GetProcAddress (GetModuleHandle (L"fwpuclnt.dll"), "FwpmNetEventSubscribe0");

						if (!_FwpmNetEventSubscribe0)
						{
							WDBG (L"GetProcAddress failed. Return value: 0x%.8lx.", GetLastError ());
						}
						else
						{
							FWPM_NET_EVENT_ENUM_TEMPLATE0 enum_template;
							FWPM_NET_EVENT_SUBSCRIPTION0 subscription;

							SecureZeroMemory (&enum_template, sizeof (enum_template));
							SecureZeroMemory (&subscription, sizeof (subscription));

							subscription.sessionKey = session.sessionKey;
							subscription.enumTemplate = &enum_template;

							result = _FwpmNetEventSubscribe0 (config.hengine, &subscription, _app_logcallback, nullptr, &config.hevent);

							if (result != ERROR_SUCCESS)
								WDBG (L"FwpmNetEventSubscribe0 failed. Return value: 0x%.8lx.", result);

							_app_loginit (TRUE); // create log file
						}
					}
				}
			}
		}

		if (!config.install_evt)
			config.install_evt = CreateEvent (nullptr, FALSE, FALSE, nullptr);

		if (!config.destroy_evt)
			config.destroy_evt = CreateEvent (nullptr, FALSE, FALSE, nullptr);

		if (!config.stop_evt)
			config.stop_evt = CreateEvent (nullptr, FALSE, FALSE, nullptr);

		if (!config.hthread)
			config.hthread = (HANDLE)_beginthreadex (nullptr, 0, &ApplyThread, nullptr, 0, nullptr);
	}
}

VOID _wfp_uninitialize ()
{
	DWORD result = 0;

	// destroy event #1
	if (config.stop_evt)
	{
		SetEvent (config.stop_evt);
		CloseHandle (config.stop_evt);
		config.stop_evt = nullptr;
	}

	// destroy event #2
	if (config.install_evt)
	{
		CloseHandle (config.install_evt);
		config.install_evt = nullptr;
	}

	// destroy event #3
	if (config.destroy_evt)
	{
		CloseHandle (config.destroy_evt);
		config.destroy_evt = nullptr;
	}

	// destroy thread
	if (config.hthread)
	{
		CloseHandle (config.hthread);
		config.hthread = nullptr;
	}

	if (config.hengine)
	{
		// net events unsubscribe (win7 and above)
		if (app.IsAdmin ())
		{
			if (_r_sys_validversion (6, 1))
			{
				_app_loginit (FALSE); // destroy log file handle if present

				if (config.hevent)
				{
					FWPMNEU0 _FwpmNetEventUnsubscribe0 = (FWPMNEU0)GetProcAddress (GetModuleHandle (L"fwpuclnt.dll"), "FwpmNetEventUnsubscribe0");

					if (!_FwpmNetEventUnsubscribe0)
					{
						WDBG (L"GetProcAddress failed. Return value: 0x%.8lx.", GetLastError ());
					}
					else
					{
						result = _FwpmNetEventUnsubscribe0 (config.hengine, config.hevent);

						if (result != ERROR_SUCCESS)
							WDBG (L"FwpmNetEventUnsubscribe0 failed. Return value: 0x%.8lx.", result);
						else
							config.hevent = nullptr;
					}
				}

				FWP_VALUE val;
				SecureZeroMemory (&val, sizeof (val));

				val.type = FWP_UINT32;
				val.uint32 = 0;

				result = FwpmEngineSetOption (config.hengine, FWPM_ENGINE_COLLECT_NET_EVENTS, &val);

				if (result != ERROR_SUCCESS)
					WDBG (L"FwpmEngineSetOption failed. Return value: 0x%.8lx.", result);
			}
		}

		FwpmEngineClose (config.hengine);
		config.hengine = nullptr;
	}
}

INT_PTR CALLBACK DlgProc (HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam)
{
	if (msg == WM_FINDMSGSTRING)
	{
		LPFINDREPLACE const lpfr = (LPFINDREPLACE)lparam;

		if ((lpfr->Flags & FR_DIALOGTERM) != 0)
		{
			config.hfind = nullptr;
		}
		else if ((lpfr->Flags & FR_FINDNEXT) != 0)
		{
			const size_t total = _r_listview_getitemcount (hwnd, IDC_LISTVIEW);
			const INT start = (INT)SendDlgItemMessage (hwnd, IDC_LISTVIEW, LVM_GETNEXTITEM, (WPARAM)total - 1, LVNI_SELECTED | LVNI_DIRECTIONMASK | LVNI_BELOW) + 1;

			for (size_t i = start; i < total; i++)
			{
				const size_t hash = _r_listview_getlparam (hwnd, IDC_LISTVIEW, i);

				ITEM_APPLICATION* const ptr = &apps[hash];

				if (StrStrI (ptr->display_path, lpfr->lpstrFindWhat) != nullptr)
				{
					ShowItem (hwnd, IDC_LISTVIEW, i);
					break;
				}
			}
		}

		return FALSE;
	}

	static DWORD max_width = 0;
	static DWORD max_height = 0;

	switch (msg)
	{
		case WM_INITDIALOG:
		{
			// static initializer
			config.wd_length = GetWindowsDirectory (config.windows_dir, _countof (config.windows_dir));
			StringCchPrintf (config.apps_path, _countof (config.apps_path), L"%s\\apps.xml", app.GetProfileDirectory ());
			StringCchPrintf (config.blocklist_path, _countof (config.blocklist_path), L"%s\\blocklist.xml", app.GetProfileDirectory ());
			StringCchPrintf (config.rules_system_path, _countof (config.rules_system_path), L"%s\\rules_system.xml", app.GetProfileDirectory ());
			StringCchPrintf (config.rules_custom_path, _countof (config.rules_custom_path), L"%s\\rules_custom.xml", app.GetProfileDirectory ());
			StringCchCopy (config.svchost_path, _countof (config.svchost_path), _r_path_expand (PATH_SVCHOST));

			config.my_hash = _r_str_hash (app.GetBinaryPath ());
			config.ntoskrnl_hash = _r_str_hash (PROC_SYSTEM_NAME);
			config.svchost_hash = _r_str_hash (config.svchost_path);

			// set privileges
			if (app.IsAdmin ())
				_r_sys_setprivilege (SE_DEBUG_NAME, TRUE);

			// configure listview
			_r_listview_setstyle (hwnd, IDC_LISTVIEW, LVS_EX_DOUBLEBUFFER | LVS_EX_FULLROWSELECT | LVS_EX_INFOTIP | LVS_EX_LABELTIP | LVS_EX_CHECKBOXES);
			_r_listview_addcolumn (hwnd, IDC_LISTVIEW, nullptr, 95, 0, LVCFMT_LEFT);

			SetIconsSize (hwnd);

			{
				const INT cx = GetSystemMetrics (SM_CXSMICON);

				config.himg = ImageList_Create (cx, cx, ILC_COLOR32 | ILC_MASK, 0, 5);

				HICON hico = _r_loadicon (app.GetHINSTANCE (), MAKEINTRESOURCE (IDI_ALLOW), cx);
				ImageList_ReplaceIcon (config.himg, -1, hico);
				config.bmp_permit = _app_ico2bmp (hico);
				DestroyIcon (hico);

				hico = _r_loadicon (app.GetHINSTANCE (), MAKEINTRESOURCE (IDI_BLOCK), cx);
				ImageList_ReplaceIcon (config.himg, -1, hico);
				config.bmp_block = _app_ico2bmp (hico);
				DestroyIcon (hico);
			}

			// uac indicator
			if (_r_sys_uacstate ())
				SendDlgItemMessage (hwnd, IDC_START_BTN, BCM_SETSHIELD, 0, TRUE);

			// drag & drop support
			DragAcceptFiles (hwnd, TRUE);

			// resize support
			RECT rc = {0};
			GetClientRect (GetDlgItem (hwnd, IDC_STATUSBAR), &rc);
			config.statusbar_height = rc.bottom;

			GetWindowRect (hwnd, &rc);

			max_width = (rc.right - rc.left);
			max_height = (rc.bottom - rc.top);

			GetClientRect (hwnd, &rc);
			SendMessage (hwnd, WM_SIZE, 0, MAKELPARAM ((rc.right - rc.left), (rc.bottom - rc.top)));

			// settings
			app.AddSettingsPage (nullptr, IDD_SETTINGS_1, IDS_SETTINGS_1, L"IDS_SETTINGS_1", &settings_callback);
			app.AddSettingsPage (nullptr, IDD_SETTINGS_3, IDS_SETTINGS_3, L"IDS_SETTINGS_3", &settings_callback);

			const size_t page_id = app.AddSettingsPage (nullptr, IDD_SETTINGS_2, IDS_TRAY_FILTERS, L"IDS_TRAY_FILTERS", &settings_callback);

			app.AddSettingsPage (nullptr, IDD_SETTINGS_7, IDS_TRAY_BLOCKLIST_RULES, L"IDS_TRAY_BLOCKLIST_RULES", &settings_callback, page_id);
			app.AddSettingsPage (nullptr, IDD_SETTINGS_5, IDS_TRAY_SYSTEM_RULES, L"IDS_TRAY_SYSTEM_RULES", &settings_callback, page_id);
			app.AddSettingsPage (nullptr, IDD_SETTINGS_6, IDS_TRAY_CUSTOM_RULES, L"IDS_TRAY_CUSTOM_RULES", &settings_callback, page_id);

			app.AddSettingsPage (nullptr, IDD_SETTINGS_4, IDS_TRAY_LOG, L"IDS_TRAY_LOG", &settings_callback);

			_wfp_initialize ();

			SetFocus (nullptr);

			break;
		}

		case WM_DROPFILES:
		{
			UINT numfiles = DragQueryFile ((HDROP)wparam, 0xFFFFFFFF, nullptr, 0);
			size_t item = 0;

			for (UINT i = 0; i < numfiles; i++)
			{
				UINT lenname = DragQueryFile ((HDROP)wparam, i, nullptr, 0);

				LPWSTR file = new WCHAR[(lenname + 1) * sizeof (WCHAR)];

				DragQueryFile ((HDROP)wparam, i, file, lenname + 1);

				item = _app_addapplication (hwnd, file, 0, FALSE);

				delete[] file;
			}

			_app_listviewsort (hwnd);
			_app_profilesave (hwnd);

			ShowItem (hwnd, IDC_LISTVIEW, _app_getposition (hwnd, item));

			DragFinish ((HDROP)wparam);

			break;
		}

		case WM_CLOSE:
		{
			if (app.ConfigGet (L"ConfirmExit", TRUE).AsBool ())
			{
				WCHAR flag[64] = {0};
				WCHAR text[128] = {0};

				INT result = 0;
				BOOL is_flagchecked = 0;

				TASKDIALOGCONFIG tdc = {0};

				tdc.cbSize = sizeof (tdc);
				tdc.dwFlags = TDF_ENABLE_HYPERLINKS | TDF_ALLOW_DIALOG_CANCELLATION | TDF_SIZE_TO_CONTENT | TDF_VERIFICATION_FLAG_CHECKED;
				tdc.hwndParent = hwnd;
				tdc.pszWindowTitle = APP_NAME;
				tdc.pfCallback = &_r_msg_callback;
				tdc.pszMainIcon = TD_INFORMATION_ICON;
				tdc.dwCommonButtons = TDCBF_YES_BUTTON | TDCBF_NO_BUTTON;
				tdc.pszMainInstruction = text;
				tdc.pszVerificationText = flag;

				StringCchCopy (text, _countof (text), I18N (&app, IDS_QUESTION_EXIT, 0));
				StringCchCopy (flag, _countof (flag), I18N (&app, IDS_ALWAYSPERFORMTHISCHECK_CHK, 0));

				TaskDialogIndirect (&tdc, &result, nullptr, &is_flagchecked);

				if (result != IDYES)
					return TRUE;

				app.ConfigSet (L"ConfirmExit", is_flagchecked);
			}

			DestroyWindow (hwnd);

			break;
		}

		case WM_DESTROY:
		{
			ImageList_Destroy (config.himg);

			DeleteObject (config.bmp_permit);
			DeleteObject (config.bmp_block);

			_wfp_uninitialize ();

			PostQuitMessage (0);

			break;
		}

		case WM_PAINT:
		{
			PAINTSTRUCT ps = {0};
			HDC dc = BeginPaint (hwnd, &ps);

			RECT rc = {0};
			GetWindowRect (GetDlgItem (hwnd, IDC_LISTVIEW), &rc);

			for (INT i = 0; i < rc.right; i++)
				SetPixel (dc, i, rc.bottom - rc.top, GetSysColor (COLOR_APPWORKSPACE));

			EndPaint (hwnd, &ps);

			break;
		}

		case WM_NOTIFY:
		{
			LPNMHDR nmlp = (LPNMHDR)lparam;

			switch (nmlp->code)
			{
				case NM_CUSTOMDRAW:
				{
					if (nmlp->idFrom == IDC_LISTVIEW)
					{
						LONG result = CDRF_DODEFAULT;
						LPNMLVCUSTOMDRAW lpnmlv = (LPNMLVCUSTOMDRAW)lparam;

						switch (lpnmlv->nmcd.dwDrawStage)
						{
							case CDDS_PREPAINT:
							{
								result = CDRF_NOTIFYITEMDRAW;
								break;
							}

							case CDDS_ITEMPREPAINT:
							{
								const size_t hash = lpnmlv->nmcd.lItemlParam;

								if (hash)
								{
									ITEM_APPLICATION const * ptr = &apps[hash];
									COLORREF new_clr = 0;

									if (app.ConfigGet (L"IsHighlightInvalid", TRUE).AsBool () && ((ptr->is_checked && !ptr->is_success) || (!ptr->is_checked && !_r_fs_exists (ptr->real_path))))
									{
										new_clr = app.ConfigGet (L"ColorInvalid", LISTVIEW_COLOR_INVALID).AsUlong ();
									}
									else if (app.ConfigGet (L"IsHighlightCustom", TRUE).AsBool () && (apps_rules.find (hash) != apps_rules.end () && !apps_rules[hash].empty ()))
									{
										new_clr = app.ConfigGet (L"ColorCustom", LISTVIEW_COLOR_CUSTOM).AsUlong ();
									}
									else if (ptr->is_silent && app.ConfigGet (L"IsHighlightSilent", TRUE).AsBool ())
									{
										new_clr = app.ConfigGet (L"ColorSilent", LISTVIEW_COLOR_SILENT).AsUlong ();
									}
									else if (ptr->is_system && app.ConfigGet (L"IsHighlightSystem", TRUE).AsBool ())
									{
										new_clr = app.ConfigGet (L"ColorSystem", LISTVIEW_COLOR_SYSTEM).AsUlong ();
									}
									else if (ptr->is_network && app.ConfigGet (L"IsHighlightNetwork", TRUE).AsBool ())
									{
										new_clr = app.ConfigGet (L"ColorNetwork", LISTVIEW_COLOR_NETWORK).AsUlong ();
									}

									if (new_clr)
									{
										_r_wnd_fillrect (lpnmlv->nmcd.hdc, &lpnmlv->nmcd.rc, new_clr);
										lpnmlv->clrTextBk = new_clr;

										result = CDRF_NEWFONT;
									}
								}

								break;
							}
						}

						SetWindowLongPtr (hwnd, DWLP_MSGRESULT, result);
						return TRUE;
					}

					break;
				}

				case LVN_GETINFOTIP:
				{
					LPNMLVGETINFOTIP lpnmlv = (LPNMLVGETINFOTIP)lparam;

					const size_t hash = (size_t)_r_listview_getlparam (hwnd, IDC_LISTVIEW, lpnmlv->iItem);

					if (hash)
					{
						ITEM_APPLICATION const* ptr = &apps[hash];

						StringCchCopy (lpnmlv->pszText, lpnmlv->cchTextMax, ptr->info);
					}

					break;
				}

				case LVN_ITEMCHANGED:
				{
					LPNMLISTVIEW lpnmlv = (LPNMLISTVIEW)lparam;

					if (lpnmlv->uNewState == 8192 || lpnmlv->uNewState == 4096)
					{
						const size_t hash = lpnmlv->lParam;

						if (!hash || !config.is_firstapply || apps.find (hash) == apps.end ())
							return FALSE;

						_R_SPINLOCK (config.lock_access);

						ITEM_APPLICATION* ptr = &apps[hash];
						ptr->is_checked = (lpnmlv->uNewState == 8192) ? TRUE : FALSE;

						_R_SPINUNLOCK (config.lock_access);

						if ((EnumMode)app.ConfigGet (L"Mode", Whitelist).AsUint () != TrustNoOne)
							SetEvent (config.install_evt); // apply filters
						else
							_app_listviewsort (hwnd);
					}

					break;
				}

				case LVN_INSERTITEM:
				case LVN_DELETEITEM:
				{
					_r_listview_resizeonecolumn (hwnd, IDC_LISTVIEW);
					_app_refreshstatus (hwnd, TRUE, FALSE);

					break;
				}

				case LVN_GETEMPTYMARKUP:
				{
					if (nmlp->idFrom == IDC_LISTVIEW)
					{
						NMLVEMPTYMARKUP* lpnmlv = (NMLVEMPTYMARKUP*)lparam;

						lpnmlv->dwFlags = EMF_CENTERED;
						StringCchCopy (lpnmlv->szMarkup, _countof (lpnmlv->szMarkup), I18N (&app, IDS_STATUS_EMPTY, 0));

						SetWindowLongPtr (hwnd, DWLP_MSGRESULT, TRUE);
						return TRUE;
					}

					break;
				}

				case NM_DBLCLK:
				{
					LPNMITEMACTIVATE lpnmlv = (LPNMITEMACTIVATE)lparam;

					if (lpnmlv->iItem != -1)
						SendMessage (hwnd, WM_COMMAND, MAKELPARAM (IDM_EXPLORE, 0), 0);

					break;
				}
			}

			break;
		}

		case WM_CONTEXTMENU:
		{
			if (GetDlgCtrlID ((HWND)wparam) == IDC_LISTVIEW)
			{
				const HMENU menu = LoadMenu (nullptr, MAKEINTRESOURCE (IDM_LISTVIEW)), submenu = GetSubMenu (menu, 0);
				const HMENU submenu1 = GetSubMenu (submenu, 1);
				const HMENU submenu3 = GetSubMenu (submenu, 3);

				// localize
				app.LocaleMenu (submenu, I18N (&app, IDS_ADD, 0), 0, TRUE);
				app.LocaleMenu (submenu, I18N (&app, IDS_ADD_FILE, 0), IDM_ADD_FILE, FALSE);
				app.LocaleMenu (submenu, I18N (&app, IDS_ADD_PROCESS, 0), 1, TRUE);
				app.LocaleMenu (submenu, I18N (&app, IDS_SETTINGS, 0), 3, TRUE);
				app.LocaleMenu (submenu, I18N (&app, IDS_DISABLENOTIFICATIONS, 0), IDM_DISABLENOTIFICATIONS, FALSE);
				app.LocaleMenu (submenu, I18N (&app, IDS_ALL, 0), IDM_ALL, FALSE);
				app.LocaleMenu (submenu, I18N (&app, IDS_REFRESH, 0) + L"\tF5", IDM_REFRESH2, FALSE);
				app.LocaleMenu (submenu, I18N (&app, IDS_EXPLORE, 0), IDM_EXPLORE, FALSE);
				app.LocaleMenu (submenu, I18N (&app, IDS_COPY, 0) + L"\tCtrl+C", IDM_COPY, FALSE);
				app.LocaleMenu (submenu, I18N (&app, IDS_DELETE, 0) + L"\tDel", IDM_DELETE, FALSE);
				app.LocaleMenu (submenu, I18N (&app, IDS_CHECK, 0), IDM_CHECK, FALSE);
				app.LocaleMenu (submenu, I18N (&app, IDS_UNCHECK, 0), IDM_UNCHECK, FALSE);

				if (!SendDlgItemMessage (hwnd, IDC_LISTVIEW, LVM_GETSELECTEDCOUNT, 0, 0))
				{
					EnableMenuItem (submenu, 3, MF_BYPOSITION | MF_DISABLED);
					EnableMenuItem (submenu, IDM_EXPLORE, MF_BYCOMMAND | MF_DISABLED);
					EnableMenuItem (submenu, IDM_COPY, MF_BYCOMMAND | MF_DISABLED);
					EnableMenuItem (submenu, IDM_DELETE, MF_BYCOMMAND | MF_DISABLED);
					EnableMenuItem (submenu, IDM_CHECK, MF_BYCOMMAND | MF_DISABLED);
					EnableMenuItem (submenu, IDM_UNCHECK, MF_BYCOMMAND | MF_DISABLED);
				}

				// generate processes popup menu
				{
					_app_getprocesslist (&processes);

					if (processes.empty ())
					{
						MENUITEMINFO mii = {0};
						rstring buffer = I18N (&app, IDS_STATUS_EMPTY2, 0);

						mii.cbSize = sizeof (mii);
						mii.fMask = MIIM_STATE | MIIM_STRING;
						mii.dwTypeData = buffer.GetBuffer ();
						mii.fState = MF_DISABLED;

						SetMenuItemInfo (submenu1, IDM_ALL, FALSE, &mii);

						buffer.Clear ();
					}
					else
					{
						AppendMenu (submenu1, MF_SEPARATOR, 0, nullptr);

						for (size_t i = 0; i < processes.size (); i++)
						{
							MENUITEMINFO mii = {0};

							mii.cbSize = sizeof (mii);
							mii.fMask = MIIM_ID | MIIM_BITMAP | MIIM_STRING;
							mii.dwTypeData = processes.at (i).display_path;
							mii.hbmpItem = processes.at (i).hbmp;
							mii.wID = IDM_PROCESS + UINT (i);

							InsertMenuItem (submenu1, IDM_PROCESS + UINT (i), FALSE, &mii);
						}
					}
				}

				// show configuration
				{
					const size_t item = (size_t)SendDlgItemMessage (hwnd, IDC_LISTVIEW, LVM_GETNEXTITEM, (WPARAM)-1, LVNI_SELECTED); // get first item
					const size_t hash = (size_t)_r_listview_getlparam (hwnd, IDC_LISTVIEW, item);

					if (hash)
					{
						ITEM_APPLICATION const* ptr = &apps[hash];

						CheckMenuItem (submenu, IDM_DISABLENOTIFICATIONS, MF_BYCOMMAND | (ptr->is_silent ? MF_CHECKED : MF_UNCHECKED));

						AppendMenu (submenu3, MF_SEPARATOR, 0, nullptr);

						if (rules_custom.empty ())
						{
							AppendMenu (submenu3, MF_STRING, IDM_RULES_APPS, I18N (&app, IDS_STATUS_EMPTY2, 0));
							EnableMenuItem (submenu3, IDM_RULES_APPS, MF_BYCOMMAND | MF_DISABLED);
						}
						else
						{
							for (size_t i = 0; i < rules_custom.size (); i++)
							{
								MENUITEMINFO mii = {0};

								const BOOL is_checked = (apps_rules.find (hash) != apps_rules.end ()) && (apps_rules[hash].find (i) != apps_rules[hash].end ());

								WCHAR buffer[128] = {0};
								StringCchPrintf (buffer, _countof (buffer), L"[%s] %s", rules_custom.at (i).is_block ? I18N (&app, IDS_ACTION_2, 0) : I18N (&app, IDS_ACTION_1, 0), rules_custom.at (i).name);

								mii.cbSize = sizeof (mii);
								mii.fMask = MIIM_ID | MIIM_STATE | MIIM_STRING;
								mii.dwTypeData = buffer;
								mii.fState = is_checked ? MF_CHECKED : MF_UNCHECKED;
								mii.wID = IDM_RULES_APPS + UINT (i);

								InsertMenuItem (submenu3, IDM_RULES_APPS + UINT (i), FALSE, &mii);
							}
						}
					}
				}

				POINT pt = {0};
				GetCursorPos (&pt);

				TrackPopupMenuEx (submenu, TPM_RIGHTBUTTON | TPM_LEFTBUTTON, pt.x, pt.y, hwnd, nullptr);

				DestroyMenu (menu);
				DestroyMenu (submenu);
			}

			break;
		}

		case WM_GETMINMAXINFO:
		{
			LPMINMAXINFO lpmmi = (LPMINMAXINFO)lparam;

			lpmmi->ptMinTrackSize.x = max_width;
			lpmmi->ptMinTrackSize.y = max_height;

			break;
		}

		case WM_SIZE:
		{
			ResizeWindow (hwnd, LOWORD (lparam), HIWORD (lparam));
			RedrawWindow (hwnd, nullptr, nullptr, RDW_ALLCHILDREN | RDW_ERASE | RDW_INVALIDATE);

			break;
		}

		case WM_TRAYICON:
		{
			switch (LOWORD (lparam))
			{
				case NIN_BALLOONUSERCLICK:
				{
					if (config.last_hash)
					{
						_r_wnd_toggle (hwnd, TRUE);

						ShowItem (hwnd, IDC_LISTVIEW, _app_getposition (hwnd, config.last_hash));

						config.last_hash = 0;
					}

					break;
				}

				case NIN_BALLOONHIDE:
				case NIN_BALLOONTIMEOUT:
				{
					config.last_hash = 0;
					break;
				}

				case WM_MBUTTONDOWN:
				{
					SendMessage (hwnd, WM_COMMAND, MAKEWPARAM (IDM_LOGSHOW, 0), 0);
					break;
				}

				case WM_LBUTTONUP:
				{
					SetForegroundWindow (hwnd);
					break;
				}

				case WM_LBUTTONDBLCLK:
				{
					_r_wnd_toggle (hwnd, FALSE);
					break;
				}

				case WM_RBUTTONUP:
				{
					SetForegroundWindow (hwnd); // don't touch

					HMENU menu = LoadMenu (nullptr, MAKEINTRESOURCE (IDM_TRAY)), submenu = GetSubMenu (menu, 0);

					// localize
					app.LocaleMenu (submenu, I18N (&app, IDS_TRAY_SHOW, 0), IDM_TRAY_SHOW, FALSE);
					app.LocaleMenu (submenu, I18N (&app, (config.is_filtersinstalled ? IDS_TRAY_STOP : IDS_TRAY_START), config.is_filtersinstalled ? L"IDS_TRAY_STOP" : L"IDS_TRAY_START"), IDM_TRAY_START, FALSE);

					app.LocaleMenu (submenu, I18N (&app, IDS_TRAY_MODE, 0), 3, TRUE);
					app.LocaleMenu (submenu, I18N (&app, IDS_MODE_WHITELIST, 0), IDM_TRAY_MODEWHITELIST, FALSE);
					app.LocaleMenu (submenu, I18N (&app, IDS_MODE_BLACKLIST, 0), IDM_TRAY_MODEBLACKLIST, FALSE);
					app.LocaleMenu (submenu, I18N (&app, IDS_MODE_TRUSTNOONE, 0), IDM_TRAY_MODETRUSTNOONE, FALSE);

					app.LocaleMenu (submenu, I18N (&app, IDS_TRAY_FILTERS, 0), 5, TRUE);

					app.LocaleMenu (submenu, I18N (&app, IDS_USEBLOCKLIST_CHK, 0), IDM_TRAY_USEBLOCKLIST_CHK, FALSE);
					app.LocaleMenu (submenu, I18N (&app, IDS_INSTALLBOOTTIMEFILTERS_CHK, 0), IDM_TRAY_INSTALLBOOTTIMEFILTERS_CHK, FALSE);
					app.LocaleMenu (submenu, _r_fmt (I18N (&app, IDS_USEUPDATECHECKING_CHK, 0), APP_NAME), IDM_TRAY_USEUPDATECHECKING_CHK, FALSE);

					app.LocaleMenu (submenu, I18N (&app, IDS_RULE_OUTBOUND_ICMP, 0), IDM_TRAY_RULE_OUTBOUND_ICMP, FALSE);
					app.LocaleMenu (submenu, I18N (&app, IDS_RULE_INBOUND_ICMP, 0), IDM_TRAY_RULE_INBOUND_ICMP, FALSE);
					app.LocaleMenu (submenu, I18N (&app, IDS_RULE_INBOUND, 0), IDM_TRAY_RULE_INBOUND, FALSE);

					app.LocaleMenu (submenu, I18N (&app, IDS_TRAY_SYSTEM_RULES, 0), 6, TRUE);
					app.LocaleMenu (submenu, I18N (&app, IDS_TRAY_CUSTOM_RULES, 0), 7, TRUE);

					app.LocaleMenu (submenu, I18N (&app, IDS_TRAY_LOG, 0), 8, TRUE);
					app.LocaleMenu (submenu, I18N (&app, IDS_ENABLELOG_CHK, 0), IDM_TRAY_ENABLELOG_CHK, FALSE);
					app.LocaleMenu (submenu, I18N (&app, IDS_ENABLENOTIFICATIONS_CHK, 0), IDM_TRAY_ENABLENOTIFICATIONS_CHK, FALSE);
					app.LocaleMenu (submenu, I18N (&app, IDS_ENABLEAPPSCOLLECTOR_CHK, 0), IDM_TRAY_ENABLEAPPSCOLLECTOR_CHK, FALSE);
					app.LocaleMenu (submenu, I18N (&app, IDS_LOGSHOW, 0), IDM_TRAY_LOGSHOW, FALSE);
					app.LocaleMenu (submenu, I18N (&app, IDS_LOGCLEAR, 0), IDM_TRAY_LOGCLEAR, FALSE);

					app.LocaleMenu (submenu, I18N (&app, IDS_SETTINGS, 0), IDM_TRAY_SETTINGS, FALSE);
					app.LocaleMenu (submenu, I18N (&app, IDS_WEBSITE, 0), IDM_TRAY_WEBSITE, FALSE);
					app.LocaleMenu (submenu, I18N (&app, IDS_ABOUT, 0), IDM_TRAY_ABOUT, FALSE);
					app.LocaleMenu (submenu, I18N (&app, IDS_EXIT, 0), IDM_TRAY_EXIT, FALSE);

					if (!app.IsAdmin ())
						EnableMenuItem (submenu, IDM_TRAY_START, MF_BYCOMMAND | MF_DISABLED);

					CheckMenuItem (submenu, IDM_TRAY_USEBLOCKLIST_CHK, MF_BYCOMMAND | (app.ConfigGet (L"UseBlocklist2", FALSE).AsBool () ? MF_CHECKED : MF_UNCHECKED));
					CheckMenuItem (submenu, IDM_TRAY_INSTALLBOOTTIMEFILTERS_CHK, MF_BYCOMMAND | (app.ConfigGet (L"InstallBoottimeFilters", FALSE).AsBool () ? MF_CHECKED : MF_UNCHECKED));
					CheckMenuItem (submenu, IDM_TRAY_USEUPDATECHECKING_CHK, MF_BYCOMMAND | (app.ConfigGet (L"AllowInternetAccess", TRUE).AsBool () ? MF_CHECKED : MF_UNCHECKED));

					CheckMenuItem (submenu, IDM_TRAY_RULE_OUTBOUND_ICMP, MF_BYCOMMAND | (app.ConfigGet (L"AllowOutboundIcmp", TRUE).AsBool () ? MF_CHECKED : MF_UNCHECKED));
					CheckMenuItem (submenu, IDM_TRAY_RULE_INBOUND_ICMP, MF_BYCOMMAND | (app.ConfigGet (L"AllowInboundIcmp", FALSE).AsBool () ? MF_CHECKED : MF_UNCHECKED));
					CheckMenuItem (submenu, IDM_TRAY_RULE_INBOUND, MF_BYCOMMAND | (app.ConfigGet (L"AllowInboundConnections", FALSE).AsBool () ? MF_CHECKED : MF_UNCHECKED));

					CheckMenuItem (submenu, IDM_TRAY_ENABLELOG_CHK, MF_BYCOMMAND | (app.ConfigGet (L"IsLogEnabled", FALSE).AsBool () ? MF_CHECKED : MF_UNCHECKED));
					CheckMenuItem (submenu, IDM_TRAY_ENABLENOTIFICATIONS_CHK, MF_BYCOMMAND | (app.ConfigGet (L"IsNotificationsEnabled", TRUE).AsBool () ? MF_CHECKED : MF_UNCHECKED));
					CheckMenuItem (submenu, IDM_TRAY_ENABLEAPPSCOLLECTOR_CHK, MF_BYCOMMAND | (app.ConfigGet (L"IsAppsCollectorEnabled", TRUE).AsBool () ? MF_CHECKED : MF_UNCHECKED));

					if (!app.IsAdmin () || !_r_sys_validversion (6, 1))
					{
						EnableMenuItem (submenu, IDM_TRAY_ENABLELOG_CHK, MF_BYCOMMAND | MF_DISABLED);
						EnableMenuItem (submenu, IDM_TRAY_ENABLENOTIFICATIONS_CHK, MF_BYCOMMAND | MF_DISABLED);
						EnableMenuItem (submenu, IDM_TRAY_ENABLEAPPSCOLLECTOR_CHK, MF_BYCOMMAND | MF_DISABLED);
					}

					CheckMenuRadioItem (submenu, IDM_TRAY_MODEWHITELIST, IDM_TRAY_MODETRUSTNOONE, IDM_TRAY_MODEWHITELIST + app.ConfigGet (L"Mode", Whitelist).AsUint (), MF_BYCOMMAND);

					// append system rules
					{
						const HMENU submenu_sub = GetSubMenu (submenu, 6);

						DeleteMenu (submenu_sub, 0, MF_BYPOSITION);

						if (rules_system.empty ())
						{
							AppendMenu (submenu_sub, MF_STRING, IDM_RULES_SYSTEM, I18N (&app, IDS_STATUS_EMPTY2, 0));
							EnableMenuItem (submenu_sub, IDM_RULES_SYSTEM, MF_BYCOMMAND | MF_DISABLED);
						}
						else
						{
							for (size_t i = 0; i < rules_system.size (); i++)
							{
								WCHAR buffer[128] = {0};
								StringCchPrintf (buffer, _countof (buffer), L"[%s] %s", rules_system.at (i).is_block ? I18N (&app, IDS_ACTION_2, 0) : I18N (&app, IDS_ACTION_1, 0), rules_system.at (i).name);

								AppendMenu (submenu_sub, MF_STRING, IDM_RULES_SYSTEM + i, buffer);

								if (app.ConfigGet (rules_system.at (i).name, rules_system.at (i).is_enabled, SECTION_SYSTEM).AsBool ())
									CheckMenuItem (submenu_sub, IDM_RULES_SYSTEM + UINT (i), MF_BYCOMMAND | MF_CHECKED);
							}
						}
					}

					// append custom rules
					{
						const HMENU submenu_sub = GetSubMenu (submenu, 7);

						DeleteMenu (submenu_sub, 0, MF_BYPOSITION);

						if (rules_custom.empty ())
						{
							AppendMenu (submenu_sub, MF_STRING, IDM_RULES_CUSTOM, I18N (&app, IDS_STATUS_EMPTY2, 0));
							EnableMenuItem (submenu_sub, IDM_RULES_CUSTOM, MF_BYCOMMAND | MF_DISABLED);
						}
						else
						{
							for (size_t i = 0; i < rules_custom.size (); i++)
							{
								WCHAR buffer[128] = {0};
								StringCchPrintf (buffer, _countof (buffer), L"[%s] %s", rules_custom.at (i).is_block ? I18N (&app, IDS_ACTION_2, 0) : I18N (&app, IDS_ACTION_1, 0), rules_custom.at (i).name);

								AppendMenu (submenu_sub, MF_STRING, IDM_RULES_CUSTOM + i, buffer);

								if (rules_custom.at (i).is_enabled)
									CheckMenuItem (submenu_sub, IDM_RULES_CUSTOM + UINT (i), MF_BYCOMMAND | MF_CHECKED);
							}
						}
					}

					POINT pt = {0};
					GetCursorPos (&pt);

					TrackPopupMenuEx (submenu, TPM_RIGHTBUTTON | TPM_LEFTBUTTON, pt.x, pt.y, hwnd, nullptr);

					DestroyMenu (submenu);
					DestroyMenu (menu);

					break;
				}
			}

			break;
		}

		case WM_DEVICECHANGE:
		{
			if (wparam == DBT_DEVICEARRIVAL)
			{
				const PDEV_BROADCAST_HDR lbhdr = (PDEV_BROADCAST_HDR)lparam;

				if (lbhdr->dbch_devicetype == DBT_DEVTYP_VOLUME)
					SetEvent (config.install_evt); // apply filters
			}

			break;
		}

		case WM_COMMAND:
		{
			if (HIWORD (wparam) == 0 && LOWORD (wparam) >= IDM_LANGUAGE && LOWORD (wparam) <= IDM_LANGUAGE + app.LocaleGetCount ())
			{
				app.LocaleApplyFromMenu (GetSubMenu (GetSubMenu (GetMenu (hwnd), 2), 7), LOWORD (wparam), IDM_LANGUAGE);

				return FALSE;
			}
			else if ((LOWORD (wparam) >= IDM_PROCESS && LOWORD (wparam) <= IDM_PROCESS + processes.size ()))
			{
				ITEM_PROCESS const * ptr = &processes.at (LOWORD (wparam) - IDM_PROCESS);

				const size_t hash = _app_addapplication (hwnd, ptr->file_path, 0, FALSE);

				_app_listviewsort (hwnd);
				_app_profilesave (hwnd);

				ShowItem (hwnd, IDC_LISTVIEW, _app_getposition (hwnd, hash));

				return FALSE;
			}
			else if ((LOWORD (wparam) >= IDM_RULES_SYSTEM && LOWORD (wparam) <= IDM_RULES_SYSTEM + rules_system.size ()))
			{
				ITEM_RULE* ptr = &rules_system.at (LOWORD (wparam) - IDM_RULES_SYSTEM);

				BOOL new_val = !app.ConfigGet (ptr->name, ptr->is_enabled, SECTION_SYSTEM).AsBool ();

				app.ConfigSet (ptr->name, new_val, SECTION_SYSTEM);
				ptr->is_enabled = new_val;

				CheckMenuItem (GetMenu (hwnd), IDM_RULES_SYSTEM + (LOWORD (wparam) - IDM_RULES_SYSTEM), MF_BYCOMMAND | (new_val ? MF_CHECKED : MF_UNCHECKED));

				SetEvent (config.install_evt); // apply filters

				return FALSE;
			}
			else if ((LOWORD (wparam) >= IDM_RULES_CUSTOM && LOWORD (wparam) <= IDM_RULES_CUSTOM + rules_custom.size ()))
			{
				ITEM_RULE* ptr = &rules_custom.at (LOWORD (wparam) - IDM_RULES_CUSTOM);

				ptr->is_enabled = !ptr->is_enabled;

				CheckMenuItem (GetMenu (hwnd), IDM_RULES_CUSTOM + (LOWORD (wparam) - IDM_RULES_CUSTOM), MF_BYCOMMAND | (ptr->is_enabled ? MF_CHECKED : MF_UNCHECKED));

				SetEvent (config.install_evt); // apply filters
				_app_profilesave (hwnd);

				return FALSE;
			}
			else if ((LOWORD (wparam) >= IDM_RULES_APPS && LOWORD (wparam) <= IDM_RULES_APPS + rules_custom.size ()))
			{
				const size_t rule_id = LOWORD (wparam) - IDM_RULES_APPS;

				const size_t item = (size_t)SendDlgItemMessage (hwnd, IDC_LISTVIEW, LVM_GETNEXTITEM, (WPARAM)-1, LVNI_SELECTED);
				const size_t hash = (size_t)_r_listview_getlparam (hwnd, IDC_LISTVIEW, item);

				if (apps_rules.find (hash) == apps_rules.end ())
				{
					apps_rules[hash][rule_id] = TRUE;
				}
				else
				{
					if (apps_rules[hash].find (rule_id) != apps_rules[hash].end ())
						apps_rules[hash].erase (rule_id);
					else
						apps_rules[hash][rule_id] = TRUE;
				}

				SetEvent (config.install_evt); // apply filters

				_app_profilesave (hwnd);

				return FALSE;
			}

			switch (LOWORD (wparam))
			{
				case IDCANCEL: // process Esc key
				case IDM_TRAY_SHOW:
				{
					_r_wnd_toggle (hwnd, FALSE);
					break;
				}

				case IDM_SETTINGS:
				case IDM_TRAY_SETTINGS:
				case IDC_SETTINGS_BTN:
				{
					app.CreateSettingsWindow ();
					break;
				}

				case IDM_EXIT:
				case IDM_TRAY_EXIT:
				case IDC_EXIT_BTN:
				{
					SendMessage (hwnd, WM_CLOSE, 0, 0);
					break;
				}

				case IDM_WEBSITE:
				case IDM_TRAY_WEBSITE:
				{
					ShellExecute (hwnd, nullptr, _APP_WEBSITE_URL, nullptr, nullptr, SW_SHOWDEFAULT);
					break;
				}

				case IDM_DONATE:
				{
					ShellExecute (hwnd, nullptr, _APP_DONATION_URL, nullptr, nullptr, SW_SHOWDEFAULT);
					break;
				}

				case IDM_CHECKUPDATES:
				{
					app.CheckForUpdates (FALSE);
					break;
				}

				case IDM_ABOUT:
				case IDM_TRAY_ABOUT:
				{
					app.CreateAboutWindow ();
					break;
				}

				case IDM_ALWAYSONTOP_CHK:
				{
					BOOL new_val = !app.ConfigGet (L"AlwaysOnTop", FALSE).AsBool ();

					CheckMenuItem (GetMenu (hwnd), IDM_ALWAYSONTOP_CHK, MF_BYCOMMAND | (new_val ? MF_CHECKED : MF_UNCHECKED));
					app.ConfigSet (L"AlwaysOnTop", new_val);

					_r_wnd_top (hwnd, new_val);

					break;
				}

				case IDM_STARTMINIMIZED_CHK:
				{
					BOOL new_val = !app.ConfigGet (L"StartMinimized", FALSE).AsBool ();

					CheckMenuItem (GetMenu (hwnd), IDM_STARTMINIMIZED_CHK, MF_BYCOMMAND | (new_val ? MF_CHECKED : MF_UNCHECKED));
					app.ConfigSet (L"StartMinimized", new_val);

					break;
				}

				case IDM_SHOWFILENAMESONLY_CHK:
				{
					BOOL new_val = !app.ConfigGet (L"ShowFilenames", TRUE).AsBool ();

					CheckMenuItem (GetMenu (hwnd), IDM_SHOWFILENAMESONLY_CHK, MF_BYCOMMAND | (new_val ? MF_CHECKED : MF_UNCHECKED));
					app.ConfigSet (L"ShowFilenames", new_val);

					_app_profileload (hwnd);

					break;
				}

				case IDM_SORTBYFNAME:
				case IDM_SORTBYFDIR:
				{
					app.ConfigSet (L"SortMode", LOWORD (wparam) == IDM_SORTBYFNAME ? 1 : 0);

					_app_listviewsort (hwnd);

					break;
				}

				case IDM_SORTISDESCEND:
				{
					app.ConfigSet (L"IsSortDescending", !app.ConfigGet (L"IsSortDescending", FALSE).AsBool ());

					_app_listviewsort (hwnd);

					break;
				}

				case IDM_ICONSSMALL:
				case IDM_ICONSLARGE:
				{
					app.ConfigSet (L"IsLargeIcons", LOWORD (wparam) == IDM_ICONSLARGE);

					SetIconsSize (hwnd);

					break;
				}

				case IDM_ICONSISHIDDEN:
				{
					app.ConfigSet (L"IsIconsHidden", !app.ConfigGet (L"IsIconsHidden", FALSE).AsBool ());

					SetIconsSize (hwnd);

					_app_profileload (hwnd);

					break;
				}

				case IDM_TRAY_MODEWHITELIST:
				case IDM_TRAY_MODEBLACKLIST:
				case IDM_TRAY_MODETRUSTNOONE:
				{
					if (app.ConfigGet (L"ConfirmModeChange", TRUE).AsBool () && _r_msg (hwnd, MB_YESNO | MB_ICONQUESTION, APP_NAME, nullptr, I18N (&app, IDS_QUESTION, 0)) != IDYES)
						break;

					EnumMode curr = Whitelist;

					if (LOWORD (wparam) == IDM_TRAY_MODEBLACKLIST)
						curr = Blacklist;
					else if (LOWORD (wparam) == IDM_TRAY_MODETRUSTNOONE)
						curr = TrustNoOne;

					app.ConfigSet (L"Mode", curr);

					_app_refreshstatus (hwnd, FALSE, TRUE);

					CheckMenuRadioItem (GetMenu (hwnd), IDM_TRAY_MODEWHITELIST, IDM_TRAY_MODETRUSTNOONE, IDM_TRAY_MODEWHITELIST + curr, MF_BYCOMMAND);

					SetEvent (config.install_evt); // apply filters

					break;
				}

				case IDM_FIND:
				{
					if (!config.hfind)
					{
						static FINDREPLACE fr = {0}; // "static" is required for WM_FINDMSGSTRING

						fr.lStructSize = sizeof (fr);
						fr.hwndOwner = hwnd;
						fr.lpstrFindWhat = config.search_string;
						fr.wFindWhatLen = _countof (config.search_string) - 1;
						fr.Flags = FR_HIDEWHOLEWORD | FR_HIDEMATCHCASE | FR_HIDEUPDOWN;

						config.hfind = FindText (&fr);
					}
					else
					{
						SetFocus (config.hfind);
					}

					break;
				}

				case IDM_FINDNEXT:
				{
					if (!config.search_string[0])
					{
						SendMessage (hwnd, WM_COMMAND, MAKEWPARAM (IDM_FIND, 0), 0);
					}
					else
					{
						FINDREPLACE fr = {0};

						fr.Flags = FR_FINDNEXT;
						fr.lpstrFindWhat = config.search_string;

						SendMessage (hwnd, WM_FINDMSGSTRING, 0, (LPARAM)&fr);
					}

					break;
				}

				case IDM_REFRESH:
				case IDM_REFRESH2:
				{
					const size_t item = (size_t)SendDlgItemMessage (hwnd, IDC_LISTVIEW, LVM_GETNEXTITEM, (WPARAM)-1, LVNI_SELECTED);

					_app_profileload (hwnd);

					ShowItem (hwnd, IDC_LISTVIEW, item);

					break;
				}

				case IDM_USEBLOCKLIST_CHK:
				case IDM_TRAY_USEBLOCKLIST_CHK:
				{
					BOOL new_val = !app.ConfigGet (L"UseBlocklist2", FALSE).AsBool ();

					CheckMenuItem (GetMenu (hwnd), IDM_USEBLOCKLIST_CHK, MF_BYCOMMAND | (new_val ? MF_CHECKED : MF_UNCHECKED));
					app.ConfigSet (L"UseBlocklist2", new_val);

					SetEvent (config.install_evt);

					break;
				}

				case IDM_INSTALLBOOTTIMEFILTERS_CHK:
				case IDM_TRAY_INSTALLBOOTTIMEFILTERS_CHK:
				{
					BOOL new_val = !app.ConfigGet (L"InstallBoottimeFilters", FALSE).AsBool ();

					CheckMenuItem (GetMenu (hwnd), IDM_INSTALLBOOTTIMEFILTERS_CHK, MF_BYCOMMAND | (new_val ? MF_CHECKED : MF_UNCHECKED));
					app.ConfigSet (L"InstallBoottimeFilters", new_val);

					SetEvent (config.install_evt);

					break;
				}

				case IDM_USEUPDATECHECKING_CHK:
				case IDM_TRAY_USEUPDATECHECKING_CHK:
				{
					BOOL new_val = !app.ConfigGet (L"AllowInternetAccess", TRUE).AsBool ();

					CheckMenuItem (GetMenu (hwnd), IDM_USEUPDATECHECKING_CHK, MF_BYCOMMAND | (new_val ? MF_CHECKED : MF_UNCHECKED));
					app.ConfigSet (L"AllowInternetAccess", new_val);

					SetEvent (config.install_evt);

					break;
				}

				case IDM_RULE_OUTBOUND_ICMP:
				case IDM_TRAY_RULE_OUTBOUND_ICMP:
				{
					BOOL new_val = !app.ConfigGet (L"AllowOutboundIcmp", TRUE).AsBool ();

					CheckMenuItem (GetMenu (hwnd), IDM_RULE_OUTBOUND_ICMP, MF_BYCOMMAND | (new_val ? MF_CHECKED : MF_UNCHECKED));
					app.ConfigSet (L"AllowOutboundIcmp", new_val);

					SetEvent (config.install_evt);

					break;
				}

				case IDM_RULE_INBOUND_ICMP:
				case IDM_TRAY_RULE_INBOUND_ICMP:
				{
					BOOL new_val = !app.ConfigGet (L"AllowInboundIcmp", FALSE).AsBool ();

					CheckMenuItem (GetMenu (hwnd), IDM_RULE_INBOUND_ICMP, MF_BYCOMMAND | (new_val ? MF_CHECKED : MF_UNCHECKED));
					app.ConfigSet (L"AllowInboundIcmp", new_val);

					SetEvent (config.install_evt);

					break;
				}

				case IDM_RULE_INBOUND:
				case IDM_TRAY_RULE_INBOUND:
				{
					BOOL new_val = !app.ConfigGet (L"AllowInboundConnections", FALSE).AsBool ();

					CheckMenuItem (GetMenu (hwnd), IDM_RULE_INBOUND, MF_BYCOMMAND | (new_val ? MF_CHECKED : MF_UNCHECKED));
					app.ConfigSet (L"AllowInboundConnections", new_val);

					SetEvent (config.install_evt);

					break;
				}

				case IDM_ENABLELOG_CHK:
				case IDM_TRAY_ENABLELOG_CHK:
				{
					BOOL new_val = !app.ConfigGet (L"IsLogEnabled", FALSE).AsBool ();

					CheckMenuItem (GetMenu (hwnd), IDM_ENABLELOG_CHK, MF_BYCOMMAND | (new_val ? MF_CHECKED : MF_UNCHECKED));
					app.ConfigSet (L"IsLogEnabled", new_val);

					_app_loginit (new_val);

					break;
				}

				case IDM_ENABLENOTIFICATIONS_CHK:
				case IDM_TRAY_ENABLENOTIFICATIONS_CHK:
				{
					BOOL new_val = !app.ConfigGet (L"IsNotificationsEnabled", TRUE).AsBool ();

					CheckMenuItem (GetMenu (hwnd), IDM_ENABLENOTIFICATIONS_CHK, MF_BYCOMMAND | (new_val ? MF_CHECKED : MF_UNCHECKED));
					app.ConfigSet (L"IsNotificationsEnabled", new_val);

					break;
				}

				case IDM_ENABLEAPPSCOLLECTOR_CHK:
				case IDM_TRAY_ENABLEAPPSCOLLECTOR_CHK:
				{
					BOOL new_val = !app.ConfigGet (L"IsAppsCollectorEnabled", TRUE).AsBool ();

					CheckMenuItem (GetMenu (hwnd), IDM_ENABLEAPPSCOLLECTOR_CHK, MF_BYCOMMAND | (new_val ? MF_CHECKED : MF_UNCHECKED));
					app.ConfigSet (L"IsAppsCollectorEnabled", new_val);

					break;
				}

				case IDM_LOGSHOW:
				case IDM_TRAY_LOGSHOW:
				{
					rstring path = _r_path_expand (app.ConfigGet (L"LogPath", PATH_LOG));

					if (!_r_fs_exists (path))
						return FALSE;

					_r_run (nullptr, _r_fmt (L"%s \"%s\"", app.ConfigGet (L"LogViewer", L"notepad.exe"), path));

					break;
				}

				case IDM_LOGCLEAR:
				case IDM_TRAY_LOGCLEAR:
				{
					rstring path = _r_path_expand (app.ConfigGet (L"LogPath", PATH_LOG));

					if (config.hlog != nullptr && config.hlog != INVALID_HANDLE_VALUE)
					{
						if (app.ConfigGet (L"ConfirmLogClear", TRUE).AsBool () && _r_msg (hwnd, MB_YESNO | MB_ICONQUESTION, APP_NAME, nullptr, I18N (&app, IDS_QUESTION, 0)) != IDYES)
							break;

						_R_SPINLOCK (config.lock_writelog);

						SetFilePointer (config.hlog, 2, nullptr, FILE_BEGIN);
						SetEndOfFile (config.hlog);

						_R_SPINUNLOCK (config.lock_writelog);
					}
					else if (_r_fs_exists (path))
					{
						if (app.ConfigGet (L"ConfirmLogClear", TRUE).AsBool () && _r_msg (hwnd, MB_YESNO | MB_ICONQUESTION, APP_NAME, nullptr, I18N (&app, IDS_QUESTION, 0)) != IDYES)
							break;

						DeleteFile (path);
					}

					break;
				}

				case IDM_TRAY_START:
				case IDC_START_BTN:
				{
					if (!app.IsAdmin ())
					{
						if (app.RunAsAdmin ())
						{
							DestroyWindow (hwnd);
							return FALSE;
						}

						app.TrayPopup (NIIF_ERROR, APP_NAME, I18N (&app, IDS_STATUS_NOPRIVILEGES, 0));
					}
					else
					{
						WCHAR text[512] = {0};
						WCHAR flag[128] = {0};

						const BOOL status = app.ConfigGet (L"IsFiltersEnabled", FALSE).AsBool ();
						INT result = 0;
						BOOL is_flagchecked = 0;

						TASKDIALOGCONFIG tdc = {0};

						tdc.cbSize = sizeof (tdc);
						tdc.dwFlags = TDF_ENABLE_HYPERLINKS | TDF_ALLOW_DIALOG_CANCELLATION | TDF_SIZE_TO_CONTENT;
						tdc.hwndParent = hwnd;
						tdc.pszWindowTitle = APP_NAME;
						tdc.pfCallback = &_r_msg_callback;
						tdc.pszMainIcon = TD_WARNING_ICON;
						tdc.dwCommonButtons = TDCBF_YES_BUTTON | TDCBF_NO_BUTTON;
						tdc.nDefaultButton = IDNO;
						tdc.pszMainInstruction = text;
						tdc.pszVerificationText = flag;

						if (status)
						{
							StringCchCopy (text, _countof (text), I18N (&app, IDS_QUESTION_STOP, 0));
							StringCchCopy (flag, _countof (flag), I18N (&app, IDS_ENABLEWINDOWSFIREWALL_CHK, 0));

							if (app.ConfigGet (L"IsEnableWindowsFirewallChecked", TRUE).AsBool ())
								tdc.dwFlags |= TDF_VERIFICATION_FLAG_CHECKED;
						}
						else
						{
							StringCchCopy (text, _countof (text), I18N (&app, IDS_QUESTION_START, 0));
							StringCchCopy (flag, _countof (flag), I18N (&app, IDS_DISABLEWINDOWSFIREWALL_CHK, 0));

							if (app.ConfigGet (L"IsDisableWindowsFirewallChecked", TRUE).AsBool ())
								tdc.dwFlags |= TDF_VERIFICATION_FLAG_CHECKED;
						}

						TaskDialogIndirect (&tdc, &result, nullptr, &is_flagchecked);

						if (result != IDYES)
							break;

						app.ConfigSet (L"IsFiltersEnabled", !status);

						if (status)
						{
							app.ConfigSet (L"IsEnableWindowsFirewallChecked", is_flagchecked);

							SetEvent (config.destroy_evt);

							if (is_flagchecked)
								Mps_ChangeConfig (FALSE);
						}
						else
						{
							app.ConfigSet (L"IsDisableWindowsFirewallChecked", is_flagchecked);

							SetEvent (config.install_evt);

							if (is_flagchecked)
								Mps_ChangeConfig (TRUE);
						}
					}

					break;
				}

				case IDM_ADD_FILE:
				{
					WCHAR files[_R_BUFFER_LENGTH] = {0};
					OPENFILENAME ofn = {0};

					size_t item = 0;

					ofn.lStructSize = sizeof (ofn);
					ofn.hwndOwner = hwnd;
					ofn.lpstrFile = files;
					ofn.nMaxFile = _countof (files);
					ofn.lpstrFilter = L"*.exe\0*.exe\0*.*\0*.*\0\0";
					ofn.Flags = OFN_EXPLORER | OFN_ALLOWMULTISELECT | OFN_ENABLESIZING | OFN_PATHMUSTEXIST | OFN_FORCESHOWHIDDEN;

					if (GetOpenFileName (&ofn))
					{
						if (files[ofn.nFileOffset - 1] != 0)
						{
							item = _app_addapplication (hwnd, files, 0, FALSE);
						}
						else
						{
							LPWSTR p = files;
							WCHAR dir[MAX_PATH] = {0};
							GetCurrentDirectory (_countof (dir), dir);

							while (*p)
							{
								p += wcslen (p) + 1;

								if (*p)
									item = _app_addapplication (hwnd, _r_fmt (L"%s\\%s", dir, p), 0, FALSE);
							}
						}

						_app_listviewsort (hwnd);
						_app_profilesave (hwnd);

						ShowItem (hwnd, IDC_LISTVIEW, _app_getposition (hwnd, _app_getposition (hwnd, item)));
					}

					break;
				}

				case IDM_ALL:
				{
					if (!processes.size ())
						_app_getprocesslist (&processes);

					for (size_t i = 0; i < processes.size (); i++)
						_app_addapplication (hwnd, processes.at (i).file_path, 0, FALSE);

					_app_listviewsort (hwnd);
					_app_profilesave (hwnd);

					break;
				}

				case IDM_EXPLORE:
				case IDM_COPY:
				case IDM_DISABLENOTIFICATIONS:
				case IDM_UNCHECK:
				case IDM_CHECK:
				{
					INT item = -1;
					BOOL new_val = BOOL (-1);

					rstring buffer;

					while ((item = (INT)SendDlgItemMessage (hwnd, IDC_LISTVIEW, LVM_GETNEXTITEM, item, LVNI_SELECTED)) != -1)
					{
						const size_t hash = (size_t)_r_listview_getlparam (hwnd, IDC_LISTVIEW, item);

						if (apps.find (hash) == apps.end ())
							continue;

						ITEM_APPLICATION* ptr = &apps[hash];

						if (LOWORD (wparam) == IDM_EXPLORE)
						{
							if (_r_fs_exists (ptr->real_path))
								_r_run (nullptr, _r_fmt (L"\"explorer.exe\" /select,\"%s\"", ptr->real_path));
							else if (_r_fs_exists (ptr->file_dir))
								ShellExecute (hwnd, nullptr, ptr->file_dir, nullptr, nullptr, SW_SHOWDEFAULT);
						}
						else if (LOWORD (wparam) == IDM_COPY)
						{
							buffer.Append (_r_listview_gettext (hwnd, IDC_LISTVIEW, item, 0)).Append (L"\r\n");
						}
						else if (LOWORD (wparam) == IDM_DISABLENOTIFICATIONS)
						{
							if (new_val == BOOL (-1))
								new_val = !ptr->is_silent;

							ptr->is_silent = new_val;

							SendDlgItemMessage (hwnd, IDC_LISTVIEW, LVM_REDRAWITEMS, item, item); // redraw (required!)
						}
						else if (LOWORD (wparam) == IDM_CHECK || LOWORD (wparam) == IDM_UNCHECK)
						{
							ptr->is_checked = LOWORD (wparam) == IDM_CHECK ? TRUE : FALSE;
							_r_listview_setcheckstate (hwnd, IDC_LISTVIEW, item, LOWORD (wparam) == IDM_CHECK ? TRUE : FALSE);
						}
					}

					if (LOWORD (wparam) == IDM_DISABLENOTIFICATIONS)
					{
						_app_profilesave (hwnd);
					}
					else if (LOWORD (wparam) == IDM_COPY)
					{
						buffer.Trim (L"\r\n");
						_r_clipboard_set (hwnd, buffer, buffer.GetLength ());
					}

					break;
				}

				case IDM_DELETE:
				{
					if (!SendDlgItemMessage (hwnd, IDC_LISTVIEW, LVM_GETSELECTEDCOUNT, 0, 0) || (app.ConfigGet (L"ConfirmDelete", TRUE).AsBool () && _r_msg (hwnd, MB_YESNO | MB_ICONQUESTION, APP_NAME, nullptr, I18N (&app, IDS_QUESTION, 0)) != IDYES))
						break;

					size_t count = _r_listview_getitemcount (hwnd, IDC_LISTVIEW) - 1;

					BOOL is_checked = FALSE;

					_R_SPINLOCK (config.lock_access);

					for (size_t i = count; i != LAST_VALUE; i--)
					{
						if (ListView_GetItemState (GetDlgItem (hwnd, IDC_LISTVIEW), i, LVNI_SELECTED))
						{
							const size_t hash = (size_t)_r_listview_getlparam (hwnd, IDC_LISTVIEW, i);

							ITEM_APPLICATION const* ptr = &apps[hash];

							if (ptr->is_checked)
								is_checked = TRUE;

							apps.erase (hash);

							SendDlgItemMessage (hwnd, IDC_LISTVIEW, LVM_DELETEITEM, i, 0);
						}
					}

					_R_SPINUNLOCK (config.lock_access);

					if (is_checked)
						SetEvent (config.install_evt); // apply filters

					_app_profilesave (hwnd);

					break;
				}

				case IDM_PURGEN:
				{
					BOOL is_deleted = FALSE;
					const size_t count = _r_listview_getitemcount (hwnd, IDC_LISTVIEW) - 1;

					_R_SPINLOCK (config.lock_access);

					for (size_t i = count; i != LAST_VALUE; i--)
					{
						const size_t hash = (size_t)_r_listview_getlparam (hwnd, IDC_LISTVIEW, i);

						ITEM_APPLICATION const* ptr = &apps[hash];

						if ((!ptr->is_checked && !ptr->is_silent) || (ptr->is_checked && !ptr->is_success) || (!ptr->is_checked && !_r_fs_exists (ptr->real_path)))
						{
							if (apps_rules.find (hash) != apps_rules.end () && !apps_rules[hash].empty ())
								continue;

							is_deleted = TRUE;

							apps.erase (hash);

							SendDlgItemMessage (hwnd, IDC_LISTVIEW, LVM_DELETEITEM, i, 0);
						}
					}

					_R_SPINUNLOCK (config.lock_access);

					if (is_deleted)
					{
						SendDlgItemMessage (hwnd, IDC_LISTVIEW, LVM_REDRAWITEMS, 0, _r_listview_getitemcount (hwnd, IDC_LISTVIEW) - 1); // redraw (required!)

						_app_profilesave (hwnd);
					}

					break;
				}

				case IDM_SELECT_ALL:
				{
					ListView_SetItemState (GetDlgItem (hwnd, IDC_LISTVIEW), -1, LVIS_SELECTED, LVIS_SELECTED);
					break;
				}
			}

			break;
		}
	}

	return FALSE;
}

INT APIENTRY wWinMain (HINSTANCE, HINSTANCE, LPWSTR, INT)
{
	if (app.CreateMainWindow (&DlgProc, &initializer_callback))
	{
		MSG msg = {0};

		HACCEL haccel = LoadAccelerators (app.GetHINSTANCE (), MAKEINTRESOURCE (IDA_MAIN));

		while (GetMessage (&msg, nullptr, 0, 0) > 0)
		{
			if ((haccel && !TranslateAccelerator (app.GetHWND (), haccel, &msg)) && !IsDialogMessage (app.GetHWND (), &msg))
			{
				TranslateMessage (&msg);
				DispatchMessage (&msg);
			}
		}

		if (haccel)
			DestroyAcceleratorTable (haccel);
	}

	return ERROR_SUCCESS;
}
