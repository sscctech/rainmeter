/*
  Copyright (C) 2011 Birunthan Mohanathas (www.poiru.net)

  This program is free software; you can redistribute it and/or
  modify it under the terms of the GNU General Public License
  as published by the Free Software Foundation; either version 2
  of the License, or (at your option) any later version.

  This program is distributed in the hope that it will be useful,
  but WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
  GNU General Public License for more details.

  You should have received a copy of the GNU General Public License
  along with this program; if not, write to the Free Software
  Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301, USA.
*/

#include "StdAfx.h"
#include "PlayerWMP.h"

CPlayer* CPlayerWMP::c_Player = NULL;
extern HINSTANCE g_Instance;

namespace {

// Definitions of ATL stuff we need to avoid dependency on atlbase.h (which is not included in free
// versions of Visual Studio).

MIDL_INTERFACE("B6EA2050-048A-11d1-82B9-00C04FB9942E")
IAxWinHostWindow : public IUnknown
{
public:
	virtual HRESULT STDMETHODCALLTYPE CreateControl(
		LPCOLESTR lpTricsData, HWND hWnd, IStream *pStream) = 0;

	virtual HRESULT STDMETHODCALLTYPE CreateControlEx(
		LPCOLESTR lpszTricsData, HWND hWnd, IStream* pStream, IUnknown** ppUnk, REFIID iidAdvise,
		IUnknown* punkSink) = 0;

	virtual HRESULT STDMETHODCALLTYPE AttachControl(IUnknown* pUnkControl, HWND hWnd) = 0;
	virtual HRESULT STDMETHODCALLTYPE QueryControl(REFIID riid, void** ppvObject) = 0;
	virtual HRESULT STDMETHODCALLTYPE SetExternalDispatch(IDispatch* pDisp) = 0;
	virtual HRESULT STDMETHODCALLTYPE SetExternalUIHandler(void* pDisp) = 0;
};

typedef BOOL (WINAPI * AtlAxWinInitFunc)();
typedef HRESULT (WINAPI * AtlAxGetControlFunc)(HWND h, IUnknown** pp);
typedef HRESULT (WINAPI * AtlAxGetHostFunc)(HWND h, IUnknown** pp);

HMODULE InitializeAtlLibrary()
{
	static HMODULE s_ATL = LoadLibrary(L"atl");
	if (s_ATL)
	{
		auto atlAxWinInit = (AtlAxWinInitFunc)GetProcAddress(s_ATL, "AtlAxWinInit");
		atlAxWinInit();
	}
	return s_ATL;
}

}  // namespace

//
// CPlayerWMP::CRemoteHost
//

CPlayerWMP::CRemoteHost::CRemoteHost() :
	m_Player(),
	m_RefCount(0)
{
}

CPlayerWMP::CRemoteHost::~CRemoteHost()
{
}

ULONG STDMETHODCALLTYPE CPlayerWMP::CRemoteHost::AddRef()
{
	++m_RefCount;
	return m_RefCount;
}

ULONG STDMETHODCALLTYPE CPlayerWMP::CRemoteHost::Release()
{
	--m_RefCount;
	if (m_RefCount == 0)
	{
		delete this;
		return 0;
	}
	return m_RefCount;
}

HRESULT STDMETHODCALLTYPE CPlayerWMP::CRemoteHost::QueryInterface(IID const& riid, void** object)
{
	if (!object)
	{
		return E_POINTER;
	}

	if (riid == __uuidof(IUnknown) ||
		riid == __uuidof(IServiceProvider))
	{
		*object = (IServiceProvider*)this;
	}
	else if (riid == __uuidof(IWMPRemoteMediaServices))
	{
		*object = (IWMPRemoteMediaServices*)this;
	}
	else if (riid == __uuidof(IWMPEvents))
	{
		*object = (IWMPEvents*)this;
	}
	else
	{
		*object = NULL;
		return E_NOINTERFACE;
	}

	AddRef();
	return S_OK;
}

HRESULT CPlayerWMP::CRemoteHost::QueryService(REFGUID guidService, REFIID riid, void** ppv)
{
	return QueryInterface(riid, ppv);
}

HRESULT CPlayerWMP::CRemoteHost::GetServiceType(BSTR* pbstrType)
{
	*pbstrType = SysAllocString(L"RemoteNoDialogs");
	return *pbstrType ? S_OK : E_POINTER;
}

HRESULT CPlayerWMP::CRemoteHost::GetApplicationName(BSTR* pbstrName)
{
	*pbstrName = SysAllocString(L"Rainmeter NowPlaying");
	return *pbstrName ? S_OK : E_POINTER;
}

HRESULT CPlayerWMP::CRemoteHost::GetScriptableObject(BSTR* pbstrName, IDispatch** ppDispatch)
{
	if (pbstrName)
	{
		*pbstrName = NULL;
	}
	if (ppDispatch)
	{
		*ppDispatch = NULL;
	}
	return E_NOTIMPL;
}

HRESULT CPlayerWMP::CRemoteHost::GetCustomUIMode(BSTR* pbstrFile)
{
	return E_POINTER;
}

/*
** Called when playing track changes.
**
*/
void CPlayerWMP::CRemoteHost::CurrentItemChange(IDispatch* pdispMedia)
{
	m_Player->m_TrackChanged = true;
}

/*
** Called when play state changes.
**
*/
void CPlayerWMP::CRemoteHost::PlayStateChange(long NewState)
{
	switch (NewState)
	{
	case wmppsStopped:
	case wmppsMediaEnded:
		m_Player->ClearData(false);
		break;

	case wmppsPaused:
		m_Player->m_State = STATE_PAUSED;
		break;

	case wmppsPlaying:
		if (m_Player->m_State == STATE_STOPPED)
		{
			m_Player->m_TrackChanged = true;
		}
		m_Player->m_State = STATE_PLAYING;
		break;

	default:
		break;
	}
}

/*
** Called when WMP quits.
**
*/
void CPlayerWMP::CRemoteHost::SwitchedToControl()
{
	m_Player->ClearData();
	m_Player->Uninitialize();
}

//
// CPlayerWMP
//

CPlayerWMP::CPlayerWMP() : CPlayer(),
	m_TrackChanged(false),
	m_Window(),
	m_LastCheckTime(0),
	m_ConnectionCookie()
{
}

CPlayerWMP::~CPlayerWMP()
{
	c_Player = NULL;
	Uninitialize();
}

/*
** Creates a shared class object.
**
*/
CPlayer* CPlayerWMP::Create()
{
	if (!c_Player)
	{
		c_Player = new CPlayerWMP();
	}

	return c_Player;
}

/*
** Set up the COM interface with WMP.
**
*/
void CPlayerWMP::Initialize()
{
	HMODULE atl = InitializeAtlLibrary();
	if (!atl)
	{
		RmLog(LOG_ERROR, L"NowPlaying: ATL not found");
		return;
	}

	auto atlAxGetControl = (AtlAxGetControlFunc)GetProcAddress(atl, "AtlAxGetControl");
	auto atlAxGetHost = (AtlAxGetHostFunc)GetProcAddress(atl, "AtlAxGetHost");

	WNDCLASS wc = {0};
	wc.hInstance = g_Instance;
	wc.lpfnWndProc = DefWindowProc;
	wc.lpszClassName = L"NowPlayingWMP";
	RegisterClass(&wc);

	// Create the container window and the ATL host window.
	m_Window = CreateWindow(
		L"NowPlayingWMP", L"",
		WS_DISABLED,
		CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT,
		NULL, NULL, g_Instance, NULL);

	HWND window = CreateWindow(
		L"AtlAxWin", L"",
		WS_DISABLED | WS_CHILD,
		CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT,
		m_Window, NULL, g_Instance, NULL);

	Microsoft::WRL::ComPtr<IUnknown> axHost;
	Microsoft::WRL::ComPtr<IObjectWithSite> hostObject;
	HRESULT hr = atlAxGetHost(window, axHost.GetAddressOf());
	if (SUCCEEDED(hr))
	{
		hr = axHost.As(&hostObject);
	}

	Microsoft::WRL::ComPtr<CRemoteHost> remoteHost(new CRemoteHost());
	if (SUCCEEDED(hr))
	{
		remoteHost->m_Player = this;
		hr = hostObject->SetSite((IWMPRemoteMediaServices*)remoteHost.Get());
	}

	Microsoft::WRL::ComPtr<IAxWinHostWindow> axWinHostWindow;
	if (SUCCEEDED(hr))
	{
		hr = axHost.As(&axWinHostWindow);
	}

	if (SUCCEEDED(hr))
	{
		hr = axWinHostWindow->CreateControl(L"{6BF52A52-394A-11D3-B153-00C04F79FAA6}", window, nullptr);
	}

	Microsoft::WRL::ComPtr<IUnknown> axControl;
	if (SUCCEEDED(hr))
	{
		hr = atlAxGetControl(window, axControl.GetAddressOf());
	}

	if (SUCCEEDED(hr))
	{
		hr = axControl.As(&m_IPlayer);
	}

	// Connect the event interface.
	Microsoft::WRL::ComPtr<IConnectionPointContainer> wmpPlayerConnectionContainer;
	if (SUCCEEDED(hr))
	{
		hr = m_IPlayer.As(&wmpPlayerConnectionContainer);
	}

	if (SUCCEEDED(hr))
	{
		hr = wmpPlayerConnectionContainer->FindConnectionPoint(
			__uuidof(IWMPEvents), m_IConnectionPoint.GetAddressOf());
	}

	if (SUCCEEDED(hr))
	{
		hr = m_IConnectionPoint->Advise(remoteHost->GetUnknown(), &m_ConnectionCookie);
		if (!m_ConnectionCookie)
		{
			hr = E_FAIL;
		}
	}

	if (SUCCEEDED(hr))
	{
		hr = m_IPlayer->get_controls(&m_IControls);
	}

	if (SUCCEEDED(hr))
	{
		hr = m_IPlayer->get_settings(&m_ISettings);
	}

	if (SUCCEEDED(hr))
	{
		WMPPlayState state;
		m_IPlayer->get_playState(&state);
		if (state == wmppsPlaying)
		{
			m_State = STATE_PLAYING;
		}
		else if (state ==  wmppsPaused)
		{
			m_State = STATE_PAUSED;
		}

		if (m_State != STATE_STOPPED)
		{
			m_TrackChanged = true;
		}

		m_Initialized = true;
	}
	else
	{
		Uninitialize();
	}
}

/*
** Close the interface with WMP.
**
*/
void CPlayerWMP::Uninitialize()
{
	if (m_Initialized)
	{
		m_Initialized = false;

		if (m_ConnectionCookie)
		{
			m_IConnectionPoint->Unadvise(m_ConnectionCookie);
			m_ConnectionCookie = 0;
		}

		m_IControls.Reset();
		m_ISettings.Reset();
		m_IConnectionPoint.Reset();
		m_IPlayer.Reset();
		DestroyWindow(m_Window);
		UnregisterClass(L"NowPlayingWMP", g_Instance);
	}
}

/*
** Called during each update of the main measure.
**
*/
void CPlayerWMP::UpdateData()
{
	if (m_Initialized)
	{
		// Get the volume
		long volume;
		m_ISettings->get_volume(&volume);
		m_Volume = (UINT)volume;

		if (m_State != STATE_STOPPED)
		{
			double position;
			m_IControls->get_currentPosition(&position);
			m_Position = (UINT)position;
		}

		if (m_TrackChanged)
		{
			m_TrackChanged = false;

			Microsoft::WRL::ComPtr<IWMPMedia> spMedia;
			m_IPlayer->get_currentMedia(spMedia.GetAddressOf());

			if (spMedia)
			{
				BSTR val;

				spMedia->getItemInfo(_bstr_t(L"Artist"), &val);
				m_Artist = val;

				spMedia->getItemInfo(_bstr_t(L"Title"), &val);
				m_Title = val;

				spMedia->getItemInfo(_bstr_t(L"Album"), &val);
				m_Album = val;

				spMedia->getItemInfo(_bstr_t(L"UserRating"), &val);
				int rating = _wtoi(val);

				if (rating > 75)
				{
					m_Rating = 5;
				}
				else if (rating > 50)
				{
					m_Rating = 4;
				}
				else if (rating > 25)
				{
					m_Rating = 3;
				}
				else if (rating > 1)
				{
					m_Rating = 2;
				}
				else
				{
					m_Rating = rating;
				}

				spMedia->getItemInfo(_bstr_t(L"WM/TrackNumber"), &val);
				m_Number = (UINT)_wtoi(val);

				spMedia->getItemInfo(_bstr_t(L"WM/Year"), &val);
				m_Year = (UINT)_wtoi(val);

				double duration;
				spMedia->get_duration(&duration);
				m_Duration = (UINT)duration;

				BSTR url;
				spMedia->get_sourceURL(&url);
				std::wstring targetPath = url;

				if (targetPath != m_FilePath)
				{
					++m_TrackCount;
					m_FilePath = targetPath;

					// Find cover if needed
					// TODO: Fix temp solution
					if (m_Measures & MEASURE_COVER || m_InstanceCount == 0)
					{
						spMedia->getItemInfo(_bstr_t(L"WM/WMCollectionID"), &val);
						targetPath.resize(targetPath.find_last_of(L'\\') + 1);
						targetPath += L"AlbumArt_";
						targetPath += val;
						targetPath += L"_Large.jpg";

						if (_waccess(targetPath.c_str(), 0) == 0)
						{
							m_CoverPath = targetPath;
						}
						else
						{
							FindCover();
						}
					}

					if (m_Measures & MEASURE_LYRICS)
					{
						FindLyrics();
					}
				}
			}
		}
	}
	else
	{
		DWORD time = GetTickCount();
		
		// Try to find WMP window every 5 seconds
		if (m_LastCheckTime = 0 || time - m_LastCheckTime > 5000)
		{
			m_LastCheckTime = time;

			if (FindWindow(L"WMPlayerApp", NULL))
			{
				Initialize();
			}
		}
	}
}

/*
** Handles the Pause bang.
**
*/
void CPlayerWMP::Pause() 
{
	m_IControls->pause();
}

/*
** Handles the Play bang.
**
*/
void CPlayerWMP::Play() 
{
	m_IControls->play();
}

/*
** Handles the Stop bang.
**
*/
void CPlayerWMP::Stop() 
{
	m_IControls->stop();
	// TODO: FIXME
	m_State = STATE_STOPPED;
}

/*
** Handles the Next bang.
**
*/
void CPlayerWMP::Next() 
{
	m_IControls->next();
}

/*
** Handles the Previous bang.
**
*/
void CPlayerWMP::Previous() 
{
	m_IControls->previous();
}

/*
** Handles the SetPosition bang.
**
*/
void CPlayerWMP::SetPosition(int position)
{
	m_IControls->put_currentPosition((double)position);
}

/*
** Handles the SetRating bang.
**
*/
void CPlayerWMP::SetRating(int rating)
{
	if (m_State != STATE_STOPPED)
	{
		Microsoft::WRL::ComPtr<IWMPMedia> spMedia;
		m_IPlayer->get_currentMedia(spMedia.GetAddressOf());

		if (spMedia)
		{
			BSTR val;
			switch (rating)
			{
			case 0:
				val = L"0";
				break;

			case 1:
				val = L"1";
				break;

			case 2:
				val = L"25";
				break;

			case 3:
				val = L"50";
				break;

			case 4:
				val = L"75";
				break;

			default: // case 5:
				val = L"99";
				break;
			}

			spMedia->setItemInfo(_bstr_t(L"UserRating"), val);
			m_Rating = rating;
		}
	}
}

/*
** Handles the SetVolume bang.
**
*/
void CPlayerWMP::SetVolume(int volume)
{
	m_ISettings->put_volume(volume);
}

/*
** Handles the ClosePlayer bang.
**
*/
void CPlayerWMP::ClosePlayer()
{
	HWND wnd = FindWindow(L"WMPlayerApp", NULL);

	if (wnd)
	{
		SendMessage(wnd, WM_CLOSE, 0, 0);
	}
}

/*
** Handles the OpenPlayer bang.
**
*/
void CPlayerWMP::OpenPlayer(std::wstring& path)
{
	ShellExecute(NULL, L"open", path.empty() ? L"wmplayer.exe" : path.c_str(), NULL, NULL, SW_SHOW);
}
