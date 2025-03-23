/* (c) Magnus Auvinen. See licence.txt in the root of the distribution for more information. */
/* If you are missing that file, acquire a complete release at teeworlds.com.                */
#include <base/system.h>
#include <engine/storage.h>
#include "linereader.h"

// compiled-in data-dir path
#define DATA_DIR "data"

class CStorage : public IStorage
{
public:
	enum
	{
		MAX_PATHS = 16
	};

	char m_aaStoragePaths[MAX_PATHS][IO_MAX_PATH_LENGTH];
	int m_NumPaths;
	char m_aDatadir[IO_MAX_PATH_LENGTH];
	char m_aUserdir[IO_MAX_PATH_LENGTH];
	char m_aCurrentdir[IO_MAX_PATH_LENGTH];
	char m_aBinarydir[IO_MAX_PATH_LENGTH];

	CStorage()
	{
		mem_zero(m_aaStoragePaths, sizeof(m_aaStoragePaths));
		m_NumPaths = 0;
		m_aDatadir[0] = 0;
		m_aUserdir[0] = 0;
	}

	int Init(const char *pApplicationName, int StorageType, int NumArgs, const char **ppArguments)
	{
		// get userdir
		fs_storage_path(pApplicationName, m_aUserdir, sizeof(m_aUserdir));

		// get datadir
		FindDatadir(ppArguments[0]);

		// get currentdir
		if(!fs_getcwd(m_aCurrentdir, sizeof(m_aCurrentdir)))
			m_aCurrentdir[0] = 0;

		// load paths from storage.cfg
		LoadPaths(ppArguments[0]);

		if(!m_NumPaths)
		{
			dbg_msg("storage", "using standard paths");
			AddDefaultPaths();
		}

		// add save directories
		if(StorageType != STORAGETYPE_BASIC && m_NumPaths && (!m_aaStoragePaths[TYPE_SAVE][0] || !fs_makedir(m_aaStoragePaths[TYPE_SAVE])))
		{
			char aPath[IO_MAX_PATH_LENGTH];
			if(StorageType == STORAGETYPE_CLIENT)
			{
				fs_makedir(GetPath(TYPE_SAVE, "screenshots", aPath, sizeof(aPath)));
				fs_makedir(GetPath(TYPE_SAVE, "screenshots/auto", aPath, sizeof(aPath)));
				fs_makedir(GetPath(TYPE_SAVE, "maps", aPath, sizeof(aPath)));
				fs_makedir(GetPath(TYPE_SAVE, "downloadedmaps", aPath, sizeof(aPath)));
			}
			fs_makedir(GetPath(TYPE_SAVE, "dumps", aPath, sizeof(aPath)));
			fs_makedir(GetPath(TYPE_SAVE, "demos", aPath, sizeof(aPath)));
			fs_makedir(GetPath(TYPE_SAVE, "demos/auto", aPath, sizeof(aPath)));
		}

		return m_NumPaths ? 0 : 1;
	}

	void LoadPaths(const char *pArgv0)
	{
		// check current directory
		IOHANDLE File = io_open("storage.cfg", IOFLAG_READ);
		if(!File)
		{
			// check usable path in argv[0]
			unsigned int Pos = ~0U;
			for(unsigned i = 0; pArgv0[i]; i++)
				if(pArgv0[i] == '/' || pArgv0[i] == '\\')
					Pos = i;
			if(Pos < IO_MAX_PATH_LENGTH)
			{
				char aBuffer[IO_MAX_PATH_LENGTH];
				str_copy(aBuffer, pArgv0, Pos+1);
				str_append(aBuffer, "/storage.cfg", sizeof(aBuffer));
				File = io_open(aBuffer, IOFLAG_READ);
			}

			if(Pos >= IO_MAX_PATH_LENGTH || !File)
			{
				dbg_msg("storage", "couldn't open storage.cfg");
				return;
			}
		}

		char *pLine;
		CLineReader LineReader;
		LineReader.Init(File);

		while((pLine = LineReader.Get()))
		{
			if(str_length(pLine) > 9 && !str_comp_num(pLine, "add_path ", 9))
				AddPath(pLine+9);
		}

		io_close(File);

		if(!m_NumPaths)
			dbg_msg("storage", "no paths found in storage.cfg");
	}

	void AddDefaultPaths()
	{
		AddPath("$USERDIR");
		AddPath("$DATADIR");
		AddPath("$CURRENTDIR");
	}

	void AddPath(const char *pPath)
	{
		if(m_NumPaths >= MAX_PATHS || !pPath[0])
			return;

		if(!str_comp(pPath, "$USERDIR"))
		{
			if(m_aUserdir[0])
			{
				str_copy(m_aaStoragePaths[m_NumPaths++], m_aUserdir, IO_MAX_PATH_LENGTH);
				dbg_msg("storage", "added path '$USERDIR' ('%s')", m_aUserdir);
			}
		}
		else if(!str_comp(pPath, "$DATADIR"))
		{
			if(m_aDatadir[0])
			{
				str_copy(m_aaStoragePaths[m_NumPaths++], m_aDatadir, IO_MAX_PATH_LENGTH);
				dbg_msg("storage", "added path '$DATADIR' ('%s')", m_aDatadir);
			}
		}
		else if(!str_comp(pPath, "$CURRENTDIR"))
		{
			m_aaStoragePaths[m_NumPaths++][0] = 0;
			dbg_msg("storage", "added path '$CURRENTDIR' ('%s')", m_aCurrentdir);
		}
		else
		{
			if(fs_is_dir(pPath))
			{
				str_copy(m_aaStoragePaths[m_NumPaths++], pPath, IO_MAX_PATH_LENGTH);
				dbg_msg("storage", "added path '%s'", pPath);
			}
		}
	}

	void FindDatadir(const char *pArgv0)
	{
		// 1) use data-dir in PWD if present
		if(fs_is_dir("data/maps"))
		{
			str_copy(m_aDatadir, "data", sizeof(m_aDatadir));
			return;
		}

		// 2) use compiled-in data-dir if present
		if(fs_is_dir(DATA_DIR "/maps"))
		{
			str_copy(m_aDatadir, DATA_DIR, sizeof(m_aDatadir));
			return;
		}

		// 3) check for usable path in argv[0]
		{
			unsigned int Pos = ~0U;
			for(unsigned i = 0; pArgv0[i]; i++)
				if(pArgv0[i] == '/' || pArgv0[i] == '\\')
					Pos = i;

			if(Pos < IO_MAX_PATH_LENGTH)
			{
				char aBaseDir[IO_MAX_PATH_LENGTH];
				str_copy(aBaseDir, pArgv0, Pos+1);
				str_format(m_aDatadir, sizeof(m_aDatadir), "%s/data", aBaseDir);
				str_append(aBaseDir, "/data/maps", sizeof(aBaseDir));

				if(fs_is_dir(aBaseDir))
					return;
				else
					m_aDatadir[0] = 0;
			}
		}

	#if defined(CONF_FAMILY_UNIX)
		// 4) check for all default locations
		{
			const char *aDirs[] = {
				"/usr/share/teeworlds/data",
				"/usr/share/games/teeworlds/data",
				"/usr/local/share/teeworlds/data",
				"/usr/local/share/games/teeworlds/data",
				"/opt/teeworlds/data"
			};
			const int DirsCount = sizeof(aDirs) / sizeof(aDirs[0]);

			int i;
			for (i = 0; i < DirsCount; i++)
			{
				char aBuf[128];
				str_format(aBuf, sizeof(aBuf), "%s/mapres", aDirs[i]);
				if(fs_is_dir(aBuf))
				{
					str_copy(m_aDatadir, aDirs[i], sizeof(m_aDatadir));
					return;
				}
			}
		}
	#endif

		// no data-dir found
		dbg_msg("storage", "warning no data directory found");
	}

	virtual void ListDirectory(int Type, const char *pPath, FS_LISTDIR_CALLBACK pfnCallback, void *pUser)
	{
		char aBuffer[IO_MAX_PATH_LENGTH];
		if(Type == TYPE_ALL)
		{
			// list all available directories
			for(int i = 0; i < m_NumPaths; ++i)
				fs_listdir(GetPath(i, pPath, aBuffer, sizeof(aBuffer)), pfnCallback, i, pUser);
		}
		else if(Type >= 0 && Type < m_NumPaths)
		{
			// list wanted directory
			fs_listdir(GetPath(Type, pPath, aBuffer, sizeof(aBuffer)), pfnCallback, Type, pUser);
		}
	}

	const char *GetPath(int Type, const char *pDir, char *pBuffer, unsigned BufferSize)
	{
		str_format(pBuffer, BufferSize, "%s%s%s", m_aaStoragePaths[Type], !m_aaStoragePaths[Type][0] ? "" : "/", pDir);
		return pBuffer;
	}

	virtual IOHANDLE OpenFile(const char *pFilename, int Flags, int Type, char *pBuffer = 0, int BufferSize = 0)
	{
		char aBuffer[IO_MAX_PATH_LENGTH];
		if(!pBuffer)
		{
			pBuffer = aBuffer;
			BufferSize = sizeof(aBuffer);
		}

		if(Flags&IOFLAG_WRITE)
		{
			return io_open(GetPath(TYPE_SAVE, pFilename, pBuffer, BufferSize), Flags);
		}
		else
		{
			IOHANDLE Handle = 0;

			if(Type == TYPE_ALL)
			{
				// check all available directories
				for(int i = 0; i < m_NumPaths; ++i)
				{
					Handle = io_open(GetPath(i, pFilename, pBuffer, BufferSize), Flags);
					if(Handle)
						return Handle;
				}
			}
			else if(Type >= 0 && Type < m_NumPaths)
			{
				// check wanted directory
				Handle = io_open(GetPath(Type, pFilename, pBuffer, BufferSize), Flags);
				if(Handle)
					return Handle;
			}
		}

		pBuffer[0] = 0;
		return 0;
	}

	struct CFindCBData
	{
		CStorage *pStorage;
		const char *pFilename;
		const char *pPath;
		char *pBuffer;
		int BufferSize;
	};

	bool ReadFile(const char *pFilename, int Type, void **ppResult, unsigned *pResultLen)
	{
		IOHANDLE File = OpenFile(pFilename, IOFLAG_READ, Type);
		if(!File)
		{
			*ppResult = 0x0;
			*pResultLen = 0;
			return false;
		}
		io_read_all(File, ppResult, pResultLen);
		io_close(File);
		return true;
	}

	char *ReadFileStr(const char *pFilename, int Type)
	{
		IOHANDLE File = OpenFile(pFilename, IOFLAG_READ | IOFLAG_SKIP_BOM, Type);
		if(!File)
		{
			return 0;
		}
		char *pResult = io_read_all_str(File);
		io_close(File);
		return pResult;
	}

	static int FindFileCallback(const char *pName, int IsDir, int Type, void *pUser)
	{
		CFindCBData Data = *static_cast<CFindCBData *>(pUser);
		if(IsDir)
		{
			if(pName[0] == '.')
				return 0;

			// search within the folder
			char aBuf[IO_MAX_PATH_LENGTH];
			char aPath[IO_MAX_PATH_LENGTH];
			str_format(aPath, sizeof(aPath), "%s/%s", Data.pPath, pName);
			Data.pPath = aPath;
			fs_listdir(Data.pStorage->GetPath(Type, aPath, aBuf, sizeof(aBuf)), FindFileCallback, Type, &Data);
			if(Data.pBuffer[0])
				return 1;
		}
		else if(!str_comp(pName, Data.pFilename))
		{
			// found the file = end
			str_format(Data.pBuffer, Data.BufferSize, "%s/%s", Data.pPath, Data.pFilename);
			return 1;
		}

		return 0;
	}

	virtual bool FindFile(const char *pFilename, const char *pPath, int Type, char *pBuffer, int BufferSize)
	{
		if(BufferSize < 1)
			return false;

		pBuffer[0] = 0;
		char aBuf[IO_MAX_PATH_LENGTH];
		CFindCBData Data;
		Data.pStorage = this;
		Data.pFilename = pFilename;
		Data.pPath = pPath;
		Data.pBuffer = pBuffer;
		Data.BufferSize = BufferSize;

		if(Type == TYPE_ALL)
		{
			// search within all available directories
			for(int i = 0; i < m_NumPaths; ++i)
			{
				fs_listdir(GetPath(i, pPath, aBuf, sizeof(aBuf)), FindFileCallback, i, &Data);
				if(pBuffer[0])
					return true;
			}
		}
		else if(Type >= 0 && Type < m_NumPaths)
		{
			// search within wanted directory
			fs_listdir(GetPath(Type, pPath, aBuf, sizeof(aBuf)), FindFileCallback, Type, &Data);
		}

		return pBuffer[0] != 0;
	}

	bool RemoveFile(const char *pFilename, int Type) override
	{
		if(Type < 0 || Type >= m_NumPaths)
			return false;

		char aBuffer[IO_MAX_PATH_LENGTH];
		return !fs_remove(GetPath(Type, pFilename, aBuffer, sizeof(aBuffer)));
	}

	virtual bool RemoveBinaryFile(const char *pFilename)
	{
		char aBuffer[IO_MAX_PATH_LENGTH];
		return !fs_remove(GetBinaryPath(pFilename, aBuffer, sizeof(aBuffer)));
	}

	virtual bool RenameFile(const char* pOldFilename, const char* pNewFilename, int Type)
	{
		if(Type < 0 || Type >= m_NumPaths)
			return false;
		char aOldBuffer[IO_MAX_PATH_LENGTH];
		char aNewBuffer[IO_MAX_PATH_LENGTH];
		return !fs_rename(GetPath(Type, pOldFilename, aOldBuffer, sizeof(aOldBuffer)), GetPath(Type, pNewFilename, aNewBuffer, sizeof (aNewBuffer)));
	}

	virtual bool RenameBinaryFile(const char* pOldFilename, const char* pNewFilename)
	{
		char aOldBuffer[IO_MAX_PATH_LENGTH];
		char aNewBuffer[IO_MAX_PATH_LENGTH];
		return !fs_rename(GetBinaryPath(pOldFilename, aOldBuffer, sizeof(aOldBuffer)), GetBinaryPath(pNewFilename, aNewBuffer, sizeof (aNewBuffer)));
	}

	bool CreateFolder(const char *pFoldername, int Type) override
	{
		if(Type < 0 || Type >= m_NumPaths)
			return false;

		char aBuffer[IO_MAX_PATH_LENGTH];
		return !fs_makedir(GetPath(Type, pFoldername, aBuffer, sizeof(aBuffer)));
	}

	void GetCompletePath(int Type, const char *pDir, char *pBuffer, unsigned BufferSize) override
	{
		if(Type < 0 || Type >= m_NumPaths)
		{
			if(BufferSize > 0)
				pBuffer[0] = 0;
			return;
		}

		GetPath(Type, pDir, pBuffer, BufferSize);
	}

	const char* GetBinaryPath(const char *pDir, char *pBuffer, unsigned BufferSize) override
	{
		str_format(pBuffer, BufferSize, "%s%s%s", m_aBinarydir, !m_aBinarydir[0] ? "" : "/", pDir);
		return pBuffer;
	}

	const char *GetDataPath(const char *pFilename, char *pBuffer, unsigned BufferSize)
	{
		str_format(pBuffer, BufferSize, "%s%s%s", m_aDatadir, !m_aDatadir[0] ? "" : "/", pFilename);
		return pBuffer;
	}

	static IStorage *Create(const char *pApplicationName, int StorageType, int NumArgs, const char **ppArguments)
	{
		CStorage *p = new CStorage();
		if(p && p->Init(pApplicationName, StorageType, NumArgs, ppArguments))
		{
			dbg_msg("storage", "initialisation failed");
			delete p;
			p = 0;
		}
		return p;
	}
};

const char *IStorage::FormatTmpPath(char *aBuf, unsigned BufSize, const char *pPath)
{
	str_format(aBuf, BufSize, "%s.%d.tmp", pPath, time(0));
	return aBuf;
}

IStorage *CreateStorage(const char *pApplicationName, int StorageType, int NumArgs, const char **ppArguments) { return CStorage::Create(pApplicationName, StorageType, NumArgs, ppArguments); }
