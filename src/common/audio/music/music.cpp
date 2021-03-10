/*
**
** music.cpp
**
** music engine
**
** Copyright 1999-2016 Randy Heit
** Copyright 2002-2016 Christoph Oelckers
**
**---------------------------------------------------------------------------
**
** Redistribution and use in source and binary forms, with or without
** modification, are permitted provided that the following conditions
** are met:
**
** 1. Redistributions of source code must retain the above copyright
**    notice, this list of conditions and the following disclaimer.
** 2. Redistributions in binary form must reproduce the above copyright
**    notice, this list of conditions and the following disclaimer in the
**    documentation and/or other materials provided with the distribution.
** 3. The name of the author may not be used to endorse or promote products
**    derived from this software without specific prior written permission.
**
** THIS SOFTWARE IS PROVIDED BY THE AUTHOR ``AS IS'' AND ANY EXPRESS OR
** IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES
** OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED.
** IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR ANY DIRECT, INDIRECT,
** INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT
** NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
** DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
** THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
** (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF
** THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
**---------------------------------------------------------------------------
**
*/

#include <stdio.h>
#include <stdlib.h>
#include <stdexcept>

#include "i_sound.h"
#include "i_music.h"
#include "printf.h"
#include "s_playlist.h"
#include "c_dispatch.h"
#include "filesystem.h"
#include "cmdlib.h"
#include "s_music.h"
#include "filereadermusicinterface.h"
#include <zmusic.h>
#include "md5.h"
#include "gain_analysis.h"
#include "gameconfigfile.h"
#include "i_specialpaths.h"

// EXTERNAL FUNCTION PROTOTYPES --------------------------------------------

extern float S_GetMusicVolume (const char *music);

static void S_ActivatePlayList(bool goBack);

// PRIVATE DATA DEFINITIONS ------------------------------------------------

static bool		MusicPaused;		// whether music is paused
MusPlayingInfo mus_playing;	// music currently being played
static FPlayList PlayList;
float	relative_volume = 1.f;
float	saved_relative_volume = 1.0f;	// this could be used to implement an ACS FadeMusic function
MusicVolumeMap MusicVolumes;
MidiDeviceMap MidiDevices;

static FileReader DefaultOpenMusic(const char* fn)
{
	// This is the minimum needed to make the music system functional.
	FileReader fr;
	fr.OpenFile(fn);
	return fr;
}
static MusicCallbacks mus_cb = { nullptr, DefaultOpenMusic };


// PUBLIC DATA DEFINITIONS -------------------------------------------------

CVAR(Bool, mus_calcgain, false, CVAR_ARCHIVE | CVAR_GLOBALCONFIG) // changing this will only take effect for the next song.
CVAR(Bool, mus_usereplaygain, true, CVAR_ARCHIVE | CVAR_GLOBALCONFIG) // changing this will only take effect for the next song.
CUSTOM_CVAR(Float, mus_gainoffset, 0.f, CVAR_ARCHIVE | CVAR_GLOBALCONFIG) // for customizing the base volume
{
	if (self > 10.f) self = 10.f;
	mus_playing.replayGainFactor = dBToAmplitude(mus_playing.replayGain + mus_gainoffset);
}

// CODE --------------------------------------------------------------------

void S_SetMusicCallbacks(MusicCallbacks* cb)
{
	mus_cb = *cb;
	if (mus_cb.OpenMusic == nullptr) mus_cb.OpenMusic = DefaultOpenMusic;	// without this we are dead in the water.
}

//==========================================================================
//
// 
//
// Create a sound system stream for the currently playing song 
//==========================================================================

static std::unique_ptr<SoundStream> musicStream;

SoundStream *S_CreateCustomStream(size_t size, int samplerate, int numchannels, StreamCallback cb, void *userdata)
{
	int flags = 0;
	if (numchannels < 2) flags |= SoundStream::Mono;
	auto stream = GSnd->CreateStream(cb, int(size), flags, samplerate, userdata);
	if (stream) stream->Play(true, 1);
	return stream;
}

void S_StopCustomStream(SoundStream *stream)
{
	if (stream)
	{
		stream->Stop();
		delete stream;
	}

}


static TArray<int16_t> convert;
static bool FillStream(SoundStream* stream, void* buff, int len, void* userdata)
{
	bool written;
	if (mus_playing.isfloat)
	{
		written = ZMusic_FillStream(mus_playing.handle, buff, len);
		if (mus_playing.replayGainFactor != 1.f)
		{
			float* fbuf = (float*)buff;
			for (int i = 0; i < len / 4; i++)
			{
				fbuf[i] *= mus_playing.replayGainFactor;
			}
		}
	}
	else
	{
		// To apply replay gain we need floating point streaming data, so 16 bit input needs to be converted here.
		convert.Resize(len / 2);
		written = ZMusic_FillStream(mus_playing.handle, convert.Data(), len/2);
		float* fbuf = (float*)buff;
		for (int i = 0; i < len / 4; i++)
		{
			fbuf[i] = convert[i] * mus_playing.replayGainFactor * (1.f/32768.f);
		}
	}
	
	if (!written)
	{
		memset((char*)buff, 0, len);
		return false;
	}
	return true;
}


void S_CreateStream()
{
	if (!mus_playing.handle) return;
	SoundStreamInfo fmt;
	ZMusic_GetStreamInfo(mus_playing.handle, &fmt);
	// always create a floating point streaming buffer so we can apply replay gain without risk of integer overflows.
	mus_playing.isfloat = fmt.mNumChannels > 0;
	if (!mus_playing.isfloat) fmt.mBufferSize *= 2;
	if (fmt.mBufferSize > 0) // if buffer size is 0 the library will play the song itself (e.g. Windows system synth.)
	{
		int flags = SoundStream::Float;
		if (abs(fmt.mNumChannels) < 2) flags |= SoundStream::Mono;

		musicStream.reset(GSnd->CreateStream(FillStream, fmt.mBufferSize, flags, fmt.mSampleRate, nullptr));
		if (musicStream) musicStream->Play(true, 1);
	}
}


void S_PauseStream(bool paused)
{
	if (musicStream) musicStream->SetPaused(paused);
}

void S_StopStream()
{
	if (musicStream)
	{
		musicStream->Stop();
		musicStream.reset();
	}
}


//==========================================================================
//
// starts playing this song
//
//==========================================================================

static bool S_StartMusicPlaying(ZMusic_MusicStream song, bool loop, float rel_vol, int subsong)
{
	if (rel_vol > 0.f && !mus_usereplaygain)
	{
		float factor = relative_volume / saved_relative_volume;
		saved_relative_volume = rel_vol;
		I_SetRelativeVolume(saved_relative_volume * factor);
	}
	ZMusic_Stop(song);
	if (!ZMusic_Start(song, subsong, loop))
	{
		return false;
	}

	// Notify the sound system of the changed relative volume
	snd_musicvolume.Callback();
	return true;
}


//==========================================================================
//
// S_PauseMusic
//
// Stop music, during game PAUSE.
//==========================================================================

void S_PauseMusic ()
{
	if (mus_playing.handle && !MusicPaused)
	{
		ZMusic_Pause(mus_playing.handle);
		S_PauseStream(true);
		MusicPaused = true;
	}
}

//==========================================================================
//
// S_ResumeMusic
//
// Resume music, after game PAUSE.
//==========================================================================

void S_ResumeMusic ()
{
	if (mus_playing.handle && MusicPaused)
	{
		ZMusic_Resume(mus_playing.handle);
		S_PauseStream(false);
		MusicPaused = false;
	}
}

//==========================================================================
//
// S_UpdateSound
//
//==========================================================================

void S_UpdateMusic ()
{
	if (mus_playing.handle != nullptr)
	{
		ZMusic_Update(mus_playing.handle);
		
		// [RH] Update music and/or playlist. IsPlaying() must be called
		// to attempt to reconnect to broken net streams and to advance the
		// playlist when the current song finishes.
		if (!ZMusic_IsPlaying(mus_playing.handle))
		{
			if (PlayList.GetNumSongs())
			{
				PlayList.Advance();
				S_ActivatePlayList(false);
			}
			else
			{
				S_StopMusic(true);
			}
		}
	}
}

//==========================================================================
//
// Resets the music player if music playback was paused.
//
//==========================================================================

void S_ResetMusic ()
{
	// stop the old music if it has been paused.
	// This ensures that the new music is started from the beginning
	// if it's the same as the last one and it has been paused.
	if (MusicPaused) S_StopMusic(true);

	// start new music for the level
	MusicPaused = false;
}


//==========================================================================
//
// S_ActivatePlayList
//
// Plays the next song in the playlist. If no songs in the playlist can be
// played, then it is deleted.
//==========================================================================

void S_ActivatePlayList (bool goBack)
{
	int startpos, pos;

	startpos = pos = PlayList.GetPosition ();
	S_StopMusic (true);
	while (!S_ChangeMusic (PlayList.GetSong (pos), 0, false, true))
	{
		pos = goBack ? PlayList.Backup () : PlayList.Advance ();
		if (pos == startpos)
		{
			PlayList.Clear();
			Printf ("Cannot play anything in the playlist.\n");
			return;
		}
	}
}

//==========================================================================
//
// S_StartMusic
//
// Starts some music with the given name.
//==========================================================================

bool S_StartMusic (const char *m_id)
{
	return S_ChangeMusic (m_id, 0, false);
}

//==========================================================================
//
// S_ChangeMusic
//
// initiates playback of a song
//
//==========================================================================
static TMap<FString, float> gainMap;

static FString ReplayGainHash(ZMusicCustomReader* reader, int flength, int playertype, const char* playparam)
{
	uint8_t buffer[50000];	// for performance reasons only hash the start of the file. If we wanted to do this to large waveform songs it'd cause noticable lag.
	uint8_t digest[16];
	char digestout[33];
	auto length = reader->read(reader, buffer, 50000);
	reader->seek(reader, 0, SEEK_SET);
	MD5Context md5;
	md5.Init();
	md5.Update(buffer, (int)length);
	md5.Final(digest);

	for (size_t j = 0; j < sizeof(digest); ++j)
	{
		sprintf(digestout + (j * 2), "%02X", digest[j]);
	}
	digestout[32] = 0;

	auto type = ZMusic_IdentifyMIDIType((uint32_t*)buffer, 32);
	if (type == MIDI_NOTMIDI) return FStringf("%d:%s", flength, digestout);
	if (playertype == -1)
	{
		// todo: get the defaults for MIDI synth and used sound font.
	}
	return FStringf("%d:%s:%d:%s", flength, digestout, playertype, playparam);
}

static void SaveGains()
{
	auto path = M_GetAppDataPath(true);
	path << "/replaygain.ini";
	FConfigFile gains(path);
	TMap<FString, float>::Iterator it(gainMap);
	TMap<FString, float>::Pair* pair;

	if (gains.SetSection("Gains", true))
	{
		while (it.NextPair(pair))
		{
			gains.SetValueForKey(pair->Key, std::to_string(pair->Value).c_str());
		}
	}
	gains.WriteConfigFile();
}

static void ReadGains()
{
	static bool done = false;
	if (done) return;
	done = true;
	auto path = M_GetAppDataPath(true);
	path << "/replaygain.ini";
	FConfigFile gains(path);
	if (gains.SetSection("Gains"))
	{
		const char* key;
		const char* value;

		while (gains.NextInSection(key, value))
		{
			gainMap.Insert(key, (float)strtod(value, nullptr));
		}
	}
}

CCMD(setreplaygain)
{
	// sets replay gain for current song to a fixed value
	if (!mus_playing.handle || mus_playing.hash.IsEmpty())
	{
		Printf("setreplaygain needs some music playing\n");
		return;
	}
	if (argv.argc() < 2)
	{
		Printf("Usage: setreplaygain {dB}\n");
		Printf("Current replay gain is %f dB\n", mus_playing.replayGain);
		return;
	}
	float dB = (float)strtod(argv[1], nullptr);
	if (dB > 10) dB = 10; // don't blast the speakers. Values above 2 or 3 are very rare.
	gainMap.Insert(mus_playing.hash, dB);
	SaveGains();
	mus_playing.replayGain = dB;
	mus_playing.replayGainFactor = (float)dBToAmplitude(mus_playing.replayGain + mus_gainoffset);
}

static void CheckReplayGain(const char *musicname, EMidiDevice playertype, const char *playparam)
{
	mus_playing.replayGain = 0.f;
	mus_playing.replayGainFactor = dBToAmplitude(mus_gainoffset);
	if (!mus_usereplaygain) return;

	FileReader reader = mus_cb.OpenMusic(musicname);
	if (!reader.isOpen()) return;
	int flength = (int)reader.GetLength();
	auto mreader = GetMusicReader(reader);	// this passes the file reader to the newly created wrapper.

	ReadGains();
	auto hash = ReplayGainHash(mreader, flength, playertype, playparam);
	mus_playing.hash = hash;
	auto entry = gainMap.CheckKey(hash);
	if (entry)
	{
		mus_playing.replayGain = *entry;
		mus_playing.replayGainFactor = dBToAmplitude(mus_playing.replayGain + mus_gainoffset);
		return;
	}
	if (!mus_calcgain) return;

	auto handle = ZMusic_OpenSong(mreader, playertype, playparam);
	if (handle == nullptr) return; // not a music file

	if (!ZMusic_Start(handle, 0, false))
	{
		ZMusic_Close(handle);
		return; // unable to open
	}

	SoundStreamInfo fmt;
	ZMusic_GetStreamInfo(handle, &fmt);
	if (fmt.mBufferSize == 0)
	{
		ZMusic_Close(handle);
		return; // external player.
	}

	int flags = SoundStream::Float;
	if (abs(fmt.mNumChannels) < 2) flags |= SoundStream::Mono;

	TArray<uint8_t> readbuffer(fmt.mBufferSize, true);
	TArray<float> lbuffer;
	TArray<float> rbuffer;
	while (ZMusic_FillStream(handle, readbuffer.Data(), fmt.mBufferSize))
	{
		unsigned index;
		// 4 cases, all with different preparation needs.
		if (fmt.mNumChannels == -2) // 16 bit stereo
		{
			int16_t* sbuf = (int16_t*)readbuffer.Data();
			int numsamples = fmt.mBufferSize / 4;
			index = lbuffer.Reserve(numsamples);
			rbuffer.Reserve(numsamples);

			for (int i = 0; i < numsamples; i++)
			{
				lbuffer[index + i] = sbuf[i * 2];
				rbuffer[index + i] = sbuf[i * 2 + 1];
			}
		}
		else if (fmt.mNumChannels == -1) // 16 bit mono
		{
			int16_t* sbuf = (int16_t*)readbuffer.Data();
			int numsamples = fmt.mBufferSize / 2;
			index = lbuffer.Reserve(numsamples);

			for (int i = 0; i < numsamples; i++)
			{
				lbuffer[index + i] = sbuf[i];
			}
		}
		else if (fmt.mNumChannels == 1) // float mono
		{
			float* sbuf = (float*)readbuffer.Data();
			int numsamples = fmt.mBufferSize / 4;
			index = lbuffer.Reserve(numsamples);
			for (int i = 0; i < numsamples; i++)
			{
				lbuffer[index + i] = sbuf[i] * 32768.f;
			}
		}
		else if (fmt.mNumChannels == 2) // float stereo
		{
			float* sbuf = (float*)readbuffer.Data();
			int numsamples = fmt.mBufferSize / 8;
			auto index = lbuffer.Reserve(numsamples);
			rbuffer.Reserve(numsamples);

			for (int i = 0; i < numsamples; i++)
			{
				lbuffer[index + i] = sbuf[i * 2] * 32768.f;
				rbuffer[index + i] = sbuf[i * 2 + 1] * 32768.f;
			}
		}
		float accTime = lbuffer.Size() / (float)fmt.mSampleRate;
		if (accTime > 8 * 60) break; // do at most 8 minutes, if the song forces a loop.
	}
	ZMusic_Close(handle);

	GainAnalyzer analyzer;
	analyzer.InitGainAnalysis(fmt.mSampleRate);
	int result = analyzer.AnalyzeSamples(lbuffer.Data(), rbuffer.Size() == 0 ? nullptr : rbuffer.Data(), lbuffer.Size(), rbuffer.Size() == 0? 1: 2);
	if (result == GAIN_ANALYSIS_OK)
	{
		auto gain = analyzer.GetTitleGain();
		Printf("Calculated replay gain for %s at %f dB\n", hash.GetChars(), gain);

		gainMap.Insert(hash, gain);
		mus_playing.replayGain = gain;
		mus_playing.replayGainFactor = dBToAmplitude(mus_playing.replayGain + mus_gainoffset);
		SaveGains();
	}
}

bool S_ChangeMusic(const char* musicname, int order, bool looping, bool force)
{
	if (nomusic) return false;	// skip the entire procedure if music is globally disabled.

	if (!force && PlayList.GetNumSongs())
	{ // Don't change if a playlist is active
		return true; // do not report an error here.
	}
	// Do game specific lookup.
	FString musicname_;
	if (mus_cb.LookupFileName)
	{
		musicname_ = mus_cb.LookupFileName(musicname, order);
		musicname = musicname_.GetChars();
	}

	if (musicname == nullptr || musicname[0] == 0)
	{
		// Don't choke if the map doesn't have a song attached
		S_StopMusic (true);
		mus_playing.name = "";
		mus_playing.LastSong = "";
		return true;
	}

	if (!mus_playing.name.IsEmpty() &&
		mus_playing.handle != nullptr &&
		stricmp(mus_playing.name, musicname) == 0 &&
		ZMusic_IsLooping(mus_playing.handle) == zmusic_bool(looping))
	{
		if (order != mus_playing.baseorder)
		{
			if (ZMusic_SetSubsong(mus_playing.handle, order))
			{
				mus_playing.baseorder = order;
			}
		}
		else if (!ZMusic_IsPlaying(mus_playing.handle))
		{
			if (!ZMusic_Start(mus_playing.handle, order, looping))
			{
				Printf("Unable to start %s: %s\n", mus_playing.name.GetChars(), ZMusic_GetLastError());
			}
			S_CreateStream();

		}
		return true;
	}

	int lumpnum = -1;
	int length = 0;
	ZMusic_MusicStream handle = nullptr;
	MidiDeviceSetting* devp = MidiDevices.CheckKey(musicname);

	// Strip off any leading file:// component.
	if (strncmp(musicname, "file://", 7) == 0)
	{
		musicname += 7;
	}

	// opening the music must be done by the game because it's different depending on the game's file system use.
	FileReader reader = mus_cb.OpenMusic(musicname);
	if (!reader.isOpen()) return false;

	// shutdown old music
	S_StopMusic(true);

	// Just record it if volume is 0 or music was disabled
	if (snd_musicvolume <= 0 || !mus_enabled)
	{
		mus_playing.loop = looping;
		mus_playing.name = musicname;
		mus_playing.baseorder = order;
		mus_playing.LastSong = musicname;
		return true;
	}

	// load & register it
	if (handle != nullptr)
	{
		mus_playing.handle = handle;
	}
	else
	{

		CheckReplayGain(musicname, devp ? (EMidiDevice)devp->device : MDEV_DEFAULT, devp ? devp->args.GetChars() : "");
		auto mreader = GetMusicReader(reader);	// this passes the file reader to the newly created wrapper.
		mus_playing.handle = ZMusic_OpenSong(mreader, devp ? (EMidiDevice)devp->device : MDEV_DEFAULT, devp ? devp->args.GetChars() : "");
		if (mus_playing.handle == nullptr)
		{
			Printf("Unable to load %s: %s\n", mus_playing.name.GetChars(), ZMusic_GetLastError());
		}
	}

	mus_playing.loop = looping;
	mus_playing.name = musicname;
	mus_playing.baseorder = 0;
	mus_playing.LastSong = "";

	if (mus_playing.handle != 0)
	{ // play it
		auto volp = MusicVolumes.CheckKey(musicname);
		float vol = volp ? *volp : 1.f;
		if (!S_StartMusicPlaying(mus_playing.handle, looping, vol, order))
		{
			Printf("Unable to start %s: %s\n", mus_playing.name.GetChars(), ZMusic_GetLastError());
			return false;
		}
		S_CreateStream();
		mus_playing.baseorder = order;
		return true;
	}
	return false;
}

//==========================================================================
//
// S_RestartMusic
//
//==========================================================================

void S_RestartMusic ()
{
	if (snd_musicvolume <= 0) return;
	if (!mus_playing.LastSong.IsEmpty() && mus_enabled)
	{
		FString song = mus_playing.LastSong;
		mus_playing.LastSong = "";
		S_ChangeMusic (song, mus_playing.baseorder, mus_playing.loop, true);
	}
	else
	{
		S_StopMusic(true);
	}
}

//==========================================================================
//
// S_MIDIDeviceChanged
//
//==========================================================================


void S_MIDIDeviceChanged(int newdev)
{
	auto song = mus_playing.handle;
	if (song != nullptr && ZMusic_IsMIDI(song) && ZMusic_IsPlaying(song))
	{
		// Reload the song to change the device
		auto mi = mus_playing;
		S_StopMusic(true);
		S_ChangeMusic(mi.name, mi.baseorder, mi.loop);
	}
}

//==========================================================================
//
// S_GetMusic
//
//==========================================================================

int S_GetMusic (const char **name)
{
	int order;

	if (mus_playing.name.IsNotEmpty())
	{
		*name = mus_playing.name;
		order = mus_playing.baseorder;
	}
	else
	{
		*name = nullptr;
		order = 0;
	}
	return order;
}

//==========================================================================
//
// S_StopMusic
//
//==========================================================================

void S_StopMusic (bool force)
{
	try
	{
		// [RH] Don't stop if a playlist is active.
		if ((force || PlayList.GetNumSongs() == 0) && !mus_playing.name.IsEmpty())
		{
			if (mus_playing.handle != nullptr)
			{
				S_ResumeMusic();
				S_StopStream();
				ZMusic_Stop(mus_playing.handle);
				auto h = mus_playing.handle;
				mus_playing.handle = nullptr;
				ZMusic_Close(h);
			}
			mus_playing.LastSong = std::move(mus_playing.name);
		}
	}
	catch (const std::runtime_error& )
	{
		//Printf("Unable to stop %s: %s\n", mus_playing.name.GetChars(), err.what());
		if (mus_playing.handle != nullptr)
		{
			auto h = mus_playing.handle;
			mus_playing.handle = nullptr;
			ZMusic_Close(h);
		}
		mus_playing.name = "";
	}
}

//==========================================================================
//
// CCMD changemus
//
//==========================================================================

CCMD (changemus)
{
	if (!nomusic)
	{
		if (argv.argc() > 1)
		{
			PlayList.Clear();
			S_ChangeMusic (argv[1], argv.argc() > 2 ? atoi (argv[2]) : 0);
		}
		else
		{
			const char *currentmus = mus_playing.name.GetChars();
			if(currentmus != nullptr && *currentmus != 0)
			{
				Printf ("currently playing %s\n", currentmus);
			}
			else
			{
				Printf ("no music playing\n");
			}
		}
	}
	else
	{
		Printf("Music is disabled\n");
	}
}

//==========================================================================
//
// CCMD stopmus
//
//==========================================================================

CCMD (stopmus)
{
	PlayList.Clear();
	S_StopMusic (false);
	mus_playing.LastSong = "";	// forget the last played song so that it won't get restarted if some volume changes occur
}

//==========================================================================
//
// CCMD playlist
//
//==========================================================================

UNSAFE_CCMD (playlist)
{
	int argc = argv.argc();

	if (argc < 2 || argc > 3)
	{
		Printf ("playlist <playlist.m3u> [<position>|shuffle]\n");
	}
	else
	{
		if (!PlayList.ChangeList(argv[1]))
		{
			Printf("Could not open " TEXTCOLOR_BOLD "%s" TEXTCOLOR_NORMAL ": %s\n", argv[1], strerror(errno));
			return;
		}
		if (PlayList.GetNumSongs () > 0)
		{
			if (argc == 3)
			{
				if (stricmp (argv[2], "shuffle") == 0)
				{
					PlayList.Shuffle ();
				}
				else
				{
					PlayList.SetPosition (atoi (argv[2]));
				}
			}
			S_ActivatePlayList (false);
		}
	}
}

//==========================================================================
//
// CCMD playlistpos
//
//==========================================================================

static bool CheckForPlaylist ()
{
	if (PlayList.GetNumSongs() == 0)
	{
		Printf ("No playlist is playing.\n");
		return false;
	}
	return true;
}

CCMD (playlistpos)
{
	if (CheckForPlaylist() && argv.argc() > 1)
	{
		PlayList.SetPosition (atoi (argv[1]) - 1);
		S_ActivatePlayList (false);
	}
}

//==========================================================================
//
// CCMD playlistnext
//
//==========================================================================

CCMD (playlistnext)
{
	if (CheckForPlaylist())
	{
		PlayList.Advance ();
		S_ActivatePlayList (false);
	}
}

//==========================================================================
//
// CCMD playlistprev
//
//==========================================================================

CCMD (playlistprev)
{
	if (CheckForPlaylist())
	{
		PlayList.Backup ();
		S_ActivatePlayList (true);
	}
}

//==========================================================================
//
// CCMD playliststatus
//
//==========================================================================

CCMD (playliststatus)
{
	if (CheckForPlaylist ())
	{
		Printf ("Song %d of %d:\n%s\n",
			PlayList.GetPosition () + 1,
			PlayList.GetNumSongs (),
			PlayList.GetSong (PlayList.GetPosition ()));
	}
}

//==========================================================================
//
// 
//
//==========================================================================

CCMD(currentmusic)
{
	if (mus_playing.name.IsNotEmpty())
	{
		Printf("Currently playing music '%s'\n", mus_playing.name.GetChars());
	}
	else
	{
		Printf("Currently no music playing\n");
	}
}
