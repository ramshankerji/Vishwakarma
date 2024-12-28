//This is our global application state. Excluding the OS Specific states.
struct tab{
	/* Tab Codes.
	0 : Unsaved Tab. All data is in memory only.
	1 : Directly Opened from local file storage.
	2 : Directly Opened from Network file storage.
	3 : Subscribed to peers 
	Tab codes.*/
	int mode = 0;

	/* In general, unless explicitely tuned, windows file full path
	including the "c:\" and terminating NULL character is 3+256+1=260
	character long. If long path is enabled, it is 2^16-1=32767
	https://learn.microsoft.com/en-us/windows/win32/fileio/maximum-file-path-limitation
	Linux also has 255 Character as file system Limit.
	https://en.wikipedia.org/wiki/Ext4
	*/
	int isShortPath = 0; //0: when it's a short path, 1 when it is long.
	char shortFileName[256]; // Example: "DesignFile.bha"
	char shortFilePath[256]; // Example: "C:\Folder1\Folder2\Folder3\"
	char* longFileName = NULL;
	char* longFilePath = NULL;

};

struct {
	int tabsCount; // First tab is numbered 0. So there is at least 1 tab always.
	int activeTabNo;
	int selfIPv4Address;
	int selfIPv4Port;
	int selfIPv6Address[2];
	int selfIPv6Port;

	tab tabs[32];
}राम;

/*
Different tabs represent different files opened in the software.
Just like different website links open in different internet browser tab.
Tab No. 0 Show the opening screen. i.e. Not associated with any particular opened file.
*/
