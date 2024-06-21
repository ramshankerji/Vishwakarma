//This is our global application state. Excluding the OS Specific states.

struct {
	int tabsCount; // First tab is numbered 0. So there is at least 1 tab always.
	int activeTabNo;
};

/*
Different tabs represent different files opened in the software.
Just like different website links open in different internet browser tab.
Tab No. 0 Show the opening screen. i.e. Not associated with any particular opened file.
*/