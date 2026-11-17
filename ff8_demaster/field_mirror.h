#pragma once
//FIELD_INTERPOLATION: the second picture. While the game draws a frame, everything it draws is drawn a second time into
//DEMASTER's own copies of the game's framebuffers, with the few things changed that make it the in-between picture
//(texturepatch_v2_fieldTween.cpp moves the layers and the characters for that second draw). Whatever the game does with
//its framebuffers afterwards (the half-size pass for transparent layers, its anti-aliasing, the final draw to the screen)
//happens to the copies too, because it is all made of the same four things: a draw, a clear, a framebuffer blit, a copy
//into a texture. A texture of the game that belongs to one of its framebuffers has a copy as well, and the second draw
//reads the copy. At the end of the frame the copy of the screen holds the in-between picture, complete.
//
//Nothing here knows about fields: it only needs to be told what the game has bound (Host) and to be called after each of
//those four things. tests/mirror_test.cpp runs it on Linux against a small frame built like the game's.

namespace fieldmirror
{
	typedef unsigned int Name; //an OpenGL framebuffer or texture

	struct Host
	{
		//what the game has bound right now
		Name (*drawFramebuffer)();
		Name (*readFramebuffer)();
		unsigned int (*activeTexture)(); //GL_TEXTURE0 + unit
		Name (*texture2D)(int unit);
		//straight to the driver (in the game, DEMASTER's own hooks sit on these calls)
		void (*bindTexture2D)(Name texture);
		void (*drawElements)(unsigned int mode, int count, unsigned int type, const void* indices);
		void (*clear)(unsigned int mask);
		void (*blit)(int srcX0, int srcY0, int srcX1, int srcY1, int dstX0, int dstY0, int dstX1, int dstY1, unsigned int mask,
			unsigned int filter);
		void (*copyTexSubImage)(unsigned int target, int level, int xOffset, int yOffset, int x, int y, int width, int height);
		void (*copyTexImage)(unsigned int target, int level, unsigned int internalFormat, int x, int y, int width, int height, int border);
		void (*texParameter)(unsigned int name, int value); //of the 2D texture bound on the active unit
		//storage for the 2D texture bound on the active unit, no pixels
		void (*texImage)(int internalFormat, int width, int height, unsigned int format, unsigned int type);
		void (*log)(const char* text);
	};
	constexpr int UNITS = 9; //texture units looked at for the game's framebuffer textures (DEMASTER's own field units end at 8)

	void Start(const Host& host); //once, with a GL context
	//The game starts a frame that gets a second picture. screenWidth x screenHeight: the window's. follows: the frame before
	//was mirrored too (otherwise the copies start again from what the game's framebuffers hold now).
	void BeginFrame(int screenWidth, int screenHeight, bool follows);
	void EndFrame();
	bool Active(); //between BeginFrame and EndFrame, and nothing failed
	bool Broken(); //something could not be copied (said in the log once): no more second pictures this session

	//Before a draw of the game goes to the driver: the copy of the framebuffer it goes to is there (a new one starts as
	//what the game's holds, which has to be read before the game draws into it).
	void BeforeDraw();
	//After the game's own draw went to the driver, with everything still bound as it was: the same draw into the copy of
	//the framebuffer it went to. The caller changes what should differ (a matrix, a vertex array) around this call.
	void Draw(unsigned int mode, int count, unsigned int type, const void* indices);
	//The same for a draw made with another call than glDrawElements: between Enter and Leave the copy of the framebuffer
	//bound for drawing takes its place, and the copies of the game's framebuffer textures theirs; the caller draws.
	struct Entered
	{
		Name game = 0;
		unsigned int active = 0;
		int count = 0, unit[UNITS] = {};
		Name texture[UNITS] = {};
	};
	bool Enter(Entered& entered); //false: nothing to draw into (not active, or no copy could be made)
	void Leave(const Entered& entered);
	//after the game's glClear, glBlitFramebuffer, glCopyTexSubImage2D, glCopyTexImage2D went to the driver: the same for the copies
	void Clear(unsigned int mask);
	void Blit(int srcX0, int srcY0, int srcX1, int srcY1, int dstX0, int dstY0, int dstX1, int dstY1, unsigned int mask, unsigned int filter);
	void CopyTexSubImage(unsigned int target, int level, int xOffset, int yOffset, int x, int y, int width, int height);
	void CopyTexImage(unsigned int target, int level, unsigned int internalFormat, int x, int y, int width, int height, int border);
	//for draws DEMASTER makes itself into one of the game's framebuffers: the copy to make them into as well (0: none)
	Name CopyOf(Name gameFramebuffer);
	//the copy of a texture of the game, when it has one (0: none)
	Name CopyOfTexture(Name gameTexture);

	//The end of the frame, when the game asks for its picture to be shown. true: the copy of the screen was drawn to this
	//frame; the game's picture is put aside and the in-between one is on the screen now (show it, wait, then ShowFrame).
	bool ShowBetween();
	void ShowFrame(); //the game's own picture back on the screen
	void Forget(); //every copy is thrown away (made again when needed)
}
