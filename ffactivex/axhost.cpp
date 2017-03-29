/* ***** BEGIN LICENSE BLOCK *****
 * Version: MPL 1.1/GPL 2.0/LGPL 2.1
 *
 * The contents of this file are subject to the Mozilla Public License Version
 * 1.1 (the "License"); you may not use this file except in compliance with
 * the License. You may obtain a copy of the License at
 * http://www.mozilla.org/MPL/
 *
 * Software distributed under the License is distributed on an "AS IS" basis,
 * WITHOUT WARRANTY OF ANY KIND, either express or implied. See the License
 * for the specific language governing rights and limitations under the
 * License.
 *
 * The Original Code is itstructures.com code.
 *
 * The Initial Developer of the Original Code is IT Structures.
 * Portions created by the Initial Developer are Copyright (C) 2008
 * the Initial Developer. All Rights Reserved.
 * 
 * Contributor:
 *                Ruediger Jungbeck <ruediger.jungbeck@rsj.de>
 *
 * Alternatively, the contents of this file may be used under the terms of
 * either the GNU General Public License Version 2 or later (the "GPL"), or
 * the GNU Lesser General Public License Version 2.1 or later (the "LGPL"),
 * in which case the provisions of the GPL or the LGPL are applicable instead
 * of those above. If you wish to allow use of your version of this file only
 * under the terms of either the GPL or the LGPL, and not to allow others to
 * use your version of this file under the terms of the MPL, indicate your
 * decision by deleting the provisions above and replace them with the notice
 * and other provisions required by the GPL or the LGPL. If you do not delete
 * the provisions above, a recipient may use your version of this file under
 * the terms of any one of the MPL, the GPL or the LGPL.
 *
 * ***** END LICENSE BLOCK ***** */

#include <comdef.h>
#include "npactivex.h"
#include "scriptable.h"

#include "common/PropertyList.h"
#include "common/PropertyBag.h"
#include "common/ItemContainer.h"
#include "common/ControlSite.h"
#include "common/ControlSiteIPFrame.h"
#include "common/ControlEventSink.h"

#include "axhost.h"

#include "HTMLDocumentContainer.h"

#ifdef NO_REGISTRY_AUTHORIZE

static const char *WellKnownProgIds[] = {
	NULL
};

static const char *WellKnownClsIds[] = {
	NULL
};

#endif

static const bool AcceptOnlyWellKnown = false;
static const bool TrustWellKnown = true;

static bool
isWellKnownProgId(const char *progid)
{
#ifdef NO_REGISTRY_AUTHORIZE

	unsigned int i = 0;

	if (!progid) {

		return false;
	}

	while (WellKnownProgIds[i]) {

		if (!strnicmp(WellKnownProgIds[i], progid, strlen(WellKnownProgIds[i]))) 
			return true;

		++i;
	}

	return false;
#else
	return true;
#endif
}

static bool
isWellKnownClsId(const char *clsid)
{
#ifdef NO_REGISTRY_AUTHORIZE
	unsigned int i = 0;

	if (!clsid) {

		return false;
	}

	while (WellKnownClsIds[i]) {

		if (!strnicmp(WellKnownClsIds[i], clsid, strlen(WellKnownClsIds[i]))) 
			return true;

		++i;
	}

	return false;
#else
	return true;
#endif
}

static LRESULT CALLBACK AxHostWinProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
	LRESULT result;

	CAxHost *host = (CAxHost *)GetWindowLong(hWnd, GWL_USERDATA);

	if (!host) {

		return DefWindowProc(hWnd, msg, wParam, lParam);
	}

	switch (msg)
	{
	case WM_SETFOCUS:
	case WM_KILLFOCUS:
	case WM_SIZE:
		if (host->Site) {

			host->Site->OnDefWindowMessage(msg, wParam, lParam, &result);
			return result;
		}
		else {

			return DefWindowProc(hWnd, msg, wParam, lParam);
		}

	// Window being destroyed
	case WM_DESTROY:
		break;

	default:
		return DefWindowProc(hWnd, msg, wParam, lParam);
	}

	return true;
}

CAxHost::CAxHost(NPP inst):
	CHost(inst),
	ClsID(CLSID_NULL),
	isValidClsID(false),
	Sink(NULL),
	Site(NULL),
	Window(NULL),
	OldProc(NULL),
	Props_(new PropertyList),
	isKnown(false),
	CodeBaseUrl(NULL),
	noWindow(false)
{
}
CAxHost::~CAxHost()
{
	np_log(instance, 0, "AxHost.~AXHost: destroying the control...");
	
	if (Window){

		if (OldProc) {
			::SetWindowLong(Window, GWL_WNDPROC, (LONG)OldProc);
			OldProc = NULL;
		}

		::SetWindowLong(Window, GWL_USERDATA, (LONG)NULL);
	}

	Clear();
	delete Props_;
}

void CAxHost::Clear() {
	if (Sink) {

		Sink->UnsubscribeFromEvents();
        Sink->Release();
		Sink = NULL;
    }

    if (Site) {

		Site->Detach();
        Site->Release();
		Site = NULL;
    }

	if (Props_) {
		Props_->Clear();
	}

	CoFreeUnusedLibraries();
}

CLSID CAxHost::ParseCLSIDFromSetting(LPCSTR str, int length) {
	CLSID ret;
	CStringW input(str, length);
	if (SUCCEEDED(CLSIDFromString(input, &ret)))
		return ret;
	int pos = input.Find(':');
	if (pos != -1) {
		CStringW wolestr(_T("{"));
		wolestr.Append(input.Mid(pos + 1));
		wolestr.Append(_T("}"));
		if (SUCCEEDED(CLSIDFromString(wolestr.GetString(), &ret)))
			return ret;
	}
	return CLSID_NULL;
}

void 
CAxHost::setWindow(HWND win)
{
	if (win != Window) {

		if (win) {
			// subclass window so we can intercept window messages and
			// do our drawing to it
			OldProc = (WNDPROC)::SetWindowLong(win, GWL_WNDPROC, (LONG)AxHostWinProc);

			// associate window with our CAxHost object so we can access 
			// it in the window procedure
			::SetWindowLong(win, GWL_USERDATA, (LONG)this);
		}
		else {
			if (OldProc)
				::SetWindowLong(Window, GWL_WNDPROC, (LONG)OldProc);

			::SetWindowLong(Window, GWL_USERDATA, (LONG)NULL);
		}

		Window = win;
	}
}

void CAxHost::ResetWindow() {
	UpdateRect(lastRect);
}

HWND 
CAxHost::getWinfow()
{
	return Window;
}

void 
CAxHost::UpdateRect(RECT rcPos)
{
	HRESULT hr = -1;
	lastRect = rcPos;

	if (Site && Window && !noWindow) {

		if (Site->GetParentWindow() == NULL) {

			hr = Site->Attach(Window, rcPos, NULL);
			if (FAILED(hr)) {
				np_log(instance, 0, "AxHost.UpdateRect: failed to attach control");
				SIZEL zero = {0, 0};
				SetRectSize(&zero);
			}
		}
		if (Site->CheckAndResetNeedUpdateContainerSize()) {
			UpdateRectSize(&rcPos);
		} else {
			Site->SetPosition(rcPos);
		}

        // Ensure clipping on parent to keep child controls happy
        ::SetWindowLong(Window, GWL_STYLE, ::GetWindowLong(Window, GWL_STYLE) | WS_CLIPCHILDREN);
	}
}

void CAxHost::setNoWindow(bool noWindow) {
	this->noWindow = noWindow;
}

void CAxHost::UpdateRectSize(LPRECT origRect) {
	if (noWindow) {
		return;
	}
	SIZEL szControl;
	if (!Site->IsVisibleAtRuntime()) {
		szControl.cx = 0;
		szControl.cy = 0;
	} else {
		if (FAILED(Site->GetControlSize(&szControl))) {
			return;
		}
	}
	SIZEL szIn;
	szIn.cx = origRect->right - origRect->left;
	szIn.cy = origRect->bottom - origRect->top;

	if (szControl.cx != szIn.cx || szControl.cy != szIn.cy) {
		SetRectSize(&szControl);
	}
}


void CAxHost::SetRectSize(LPSIZEL size) {
	np_log(instance, 1, "Set object size: x = %d, y = %d", size->cx, size->cy);
	NPObjectProxy object;
	NPNFuncs.getvalue(instance, NPNVPluginElementNPObject, &object);
	static NPIdentifier style = NPNFuncs.getstringidentifier("style");
	static NPIdentifier height = NPNFuncs.getstringidentifier("height");
	static NPIdentifier width = NPNFuncs.getstringidentifier("width");
	NPVariant sHeight, sWidth;

	CStringA strHeight, strWidth;
	strHeight.Format("%dpx", size->cy);
	strWidth.Format("%dpx", size->cx);
	STRINGZ_TO_NPVARIANT(strHeight, sHeight);
	STRINGZ_TO_NPVARIANT(strWidth, sWidth);

	NPVariantProxy styleValue;

	NPNFuncs.getproperty(instance, object, style, &styleValue);
	NPObject *styleObject = NPVARIANT_TO_OBJECT(styleValue);
				
	NPNFuncs.setproperty(instance, styleObject, height, &sHeight);
	NPNFuncs.setproperty(instance, styleObject, width, &sWidth);
}

void CAxHost::SetNPWindow(NPWindow *window) {
	
	RECT rcPos;
	setWindow((HWND)window->window);
	
	rcPos.left = 0;
	rcPos.top = 0;
	rcPos.right = window->width;
	rcPos.bottom = window->height;
	UpdateRect(rcPos);
}
bool
CAxHost::verifyClsID(LPOLESTR oleClsID)
{
	CRegKey keyExplorer;
	if (ERROR_SUCCESS == keyExplorer.Open(HKEY_LOCAL_MACHINE, 
										  _T("SOFTWARE\\Microsoft\\Internet Explorer\\ActiveX Compatibility"), 
										  KEY_READ)) {
		CRegKey keyCLSID;
        if (ERROR_SUCCESS == keyCLSID.Open(keyExplorer, W2T(oleClsID), KEY_READ)) {

			DWORD dwType = REG_DWORD;
            DWORD dwFlags = 0;
            DWORD dwBufSize = sizeof(dwFlags);
            if (ERROR_SUCCESS == ::RegQueryValueEx(keyCLSID, 
												   _T("Compatibility Flags"), 
												   NULL, 
												   &dwType, 
												   (LPBYTE) 
												   &dwFlags, 
												   &dwBufSize)) {
                // Flags for this reg key
                const DWORD kKillBit = 0x00000400;
                if (dwFlags & kKillBit) {

					np_log(instance, 0, "AxHost.verifyClsID: the control is marked as unsafe by IE kill bits");
					return false;
                }
            }
        }
    }
	return true;
}

bool 
CAxHost::setClsID(const char *clsid)
{
	HRESULT hr = -1;
	USES_CONVERSION;
	LPOLESTR oleClsID = A2OLE(clsid);

	if (isWellKnownClsId(clsid)) {

		isKnown = true;
	}
	else if (AcceptOnlyWellKnown) {

		np_log(instance, 0, "AxHost.setClsID: the requested CLSID is not on the Well Known list");
		return false;
	}

    // Check the Internet Explorer list of vulnerable controls
    if (oleClsID && verifyClsID(oleClsID)) {
		CLSID vclsid;
		hr = CLSIDFromString(oleClsID, &vclsid);
		if (SUCCEEDED(hr)) {
			return setClsID(vclsid);
		}
    }

	np_log(instance, 0, "AxHost.setClsID: failed to set the requested clsid");
	return false;
}

bool CAxHost::setClsID(const CLSID& clsid) {
	if (clsid != CLSID_NULL) {
		this->ClsID = clsid;
		isValidClsID = true;
		//np_log(instance, 1, "AxHost.setClsID: CLSID %s set", clsid);
		return true;
	}
	return false;
}

void CAxHost::setCodeBaseUrl(LPCWSTR codeBaseUrl)
{
	CodeBaseUrl = codeBaseUrl;
}

bool
CAxHost::setClsIDFromProgID(const char *progid)
{
	HRESULT hr = -1;
	CLSID clsid = CLSID_NULL;
	USES_CONVERSION;
	LPOLESTR oleClsID = NULL;
	LPOLESTR oleProgID = A2OLE(progid);

	if (AcceptOnlyWellKnown) {

		if (isWellKnownProgId(progid)) {

			isKnown = true;
		}
		else {

			np_log(instance, 0, "AxHost.setClsIDFromProgID: the requested PROGID is not on the Well Known list");
			return false;
		}
	}

	hr = CLSIDFromProgID(oleProgID, &clsid);
	if (FAILED(hr)) {

		np_log(instance, 0, "AxHost.setClsIDFromProgID: could not resolve PROGID");
		return false;
	}

	hr = StringFromCLSID(clsid, &oleClsID);

    // Check the Internet Explorer list of vulnerable controls
    if (   SUCCEEDED(hr) 
		&& oleClsID 
		&& verifyClsID(oleClsID)) {

		ClsID = clsid;
		if (!::IsEqualCLSID(ClsID, CLSID_NULL)) {

			isValidClsID = true;
			np_log(instance, 1, "AxHost.setClsIDFromProgID: PROGID %s resolved and set", progid);
			return true;
		}
    }

	np_log(instance, 0, "AxHost.setClsIDFromProgID: failed to set the resolved CLSID");
	return false;
}

bool 
CAxHost::hasValidClsID()
{
	return isValidClsID;
}

static void HTMLContainerDeleter(IUnknown *unk) {
	CComAggObject<HTMLDocumentContainer>* val = (CComAggObject<HTMLDocumentContainer>*)(unk);
	val->InternalRelease();
}
bool
CAxHost::CreateControl(bool subscribeToEvents)
{
	if (!isValidClsID) {

		np_log(instance, 0, "AxHost.CreateControl: current location is not trusted");
		return false;
	}

	// Create the control site
	CComObject<CControlSite>::CreateInstance(&Site);
	if (Site == NULL) {

		np_log(instance, 0, "AxHost.CreateControl: CreateInstance failed");
		return false;
	}
	Site->AddRef();
	Site->m_bSupportWindowlessActivation = false;

	if (TrustWellKnown && isKnown) {

		Site->SetSecurityPolicy(NULL);
		Site->m_bSafeForScriptingObjectsOnly = false;
	}
	else {

		Site->m_bSafeForScriptingObjectsOnly = true;
	}

	CComAggObject<HTMLDocumentContainer> *document;
	CComAggObject<HTMLDocumentContainer>::CreateInstance(Site->GetUnknown(), &document);
	document->m_contained.Init(instance, pHtmlLib);
	Site->SetInnerWindow(document, HTMLContainerDeleter);

	BSTR url;
	document->m_contained.get_LocationURL(&url);
	Site->SetUrl(url);
	SysFreeString(url);
	
	// Create the object
	HRESULT hr;
	hr = Site->Create(ClsID, *Props(), CodeBaseUrl);

	if (FAILED(hr)) {
		np_log(instance, 0, "AxHost.CreateControl: failed to create site for 0x%08x", hr);
		return false;
	}
	
//#if 0
	IUnknown *control = NULL;
	Site->GetControlUnknown(&control);
	if (!control) {

		np_log(instance, 0, "AxHost.CreateControl: failed to create control (was it just downloaded?)");
		return false;
	}
	// Create the event sink
	CComObject<CControlEventSink>::CreateInstance(&Sink);
	Sink->AddRef();
	Sink->instance = instance;
	hr = Sink->SubscribeToEvents(control);
	control->Release();

	if (FAILED(hr) && subscribeToEvents) {
		np_log(instance, 0, "AxHost.CreateControl: SubscribeToEvents failed");
		// return false;
		// It doesn't matter.
	}
//#endif
	np_log(instance, 1, "AxHost.CreateControl: control created successfully");
	return true;
}

bool 
CAxHost::AddEventHandler(wchar_t *name, wchar_t *handler)
{
	HRESULT hr;
	DISPID id = 0;
	USES_CONVERSION;
	LPOLESTR oleName = name;

	if (!Sink) {

		np_log(instance, 0, "AxHost.AddEventHandler: no valid sink");
		return false;
	}

	hr = Sink->m_spEventSinkTypeInfo->GetIDsOfNames(&oleName, 1, &id);
	if (FAILED(hr)) {

		np_log(instance, 0, "AxHost.AddEventHandler: GetIDsOfNames failed to resolve event name");
		return false;
	}

	Sink->events[id] = handler;
	np_log(instance, 1, "AxHost.AddEventHandler: handler %S set for event %S", handler, name);
	return true;
}

int16
CAxHost::HandleEvent(void *event)
{
	NPEvent *npEvent = (NPEvent *)event;
	LRESULT result = 0;

	if (!npEvent) {

		return 0;
	}

	// forward all events to the hosted control
	return (int16)Site->OnDefWindowMessage(npEvent->event, npEvent->wParam, npEvent->lParam, &result);
}

ScriptBase *
CAxHost::CreateScriptableObject()
{
	Scriptable *obj = Scriptable::FromAxHost(instance, this);
	if (Site == NULL) {
		return obj;
	}
	static int markedSafe = 0;
	if (!Site->m_bSafeForScriptingObjectsOnly && markedSafe == 0)
	{
		if (MessageBox(NULL, _T("Some objects are not script-safe, would you continue?"), _T("Warining"), MB_YESNO | MB_ICONINFORMATION | MB_ICONASTERISK) == IDYES)
			markedSafe = 1;
		else 
			markedSafe = 2;
	}
	if (!Site->m_bSafeForScriptingObjectsOnly && markedSafe != 1)
	{
		// Disable scripting.
		obj->Invalidate();
	}
	return obj;
}

HRESULT CAxHost::GetControlUnknown(IUnknown **pObj) {
	if (Site == NULL) {
		return E_FAIL;
	}
	return Site->GetControlUnknown(pObj);
}

NPP CAxHost::ResetNPP(NPP npp) {
	NPP ret = CHost::ResetNPP(npp);
	setWindow(NULL);
	Site->Detach();
	if (Sink)
		Sink->instance = npp;
	return ret;
}
