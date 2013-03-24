/*
Copyright (C) 2001 StrmnNrmn

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
Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA  02111-1307, USA.

*/

#include "stdafx.h"
#include "DLParser.h"
#include "DLDebug.h"
#include "BaseRenderer.h"
#include "N64PixelFormat.h"
#include "Graphics/NativePixelFormat.h"
#include "RDP.h"
#include "RDPStateManager.h"
#include "TextureCache.h"
#include "ConvertImage.h"			// Convert555ToRGBA
#include "Microcode.h"
#include "uCodes/UcodeDefs.h"
#include "uCodes/Ucode.h"

#include "Math/MathUtil.h"

#include "Utility/Profiler.h"
#include "Utility/IO.h"

#include "Graphics/GraphicsContext.h"
#include "Plugins/GraphicsPlugin.h"

#include "Debug/Dump.h"
#include "Debug/DBGConsole.h"

#include "Core/Memory.h"
#include "Core/ROM.h"
#include "Core/CPU.h"

#include "OSHLE/ultra_sptask.h"
#include "OSHLE/ultra_gbi.h"
#include "OSHLE/ultra_rcp.h"

#include "Test/BatchTest.h"

#include "ConfigOptions.h"

#include <vector>

//*****************************************************************************
//
//*****************************************************************************
#ifdef DAEDALUS_DEBUG_DISPLAYLIST
#define DL_UNIMPLEMENTED_ERROR( msg )			\
{												\
	static bool shown = false;					\
	if (!shown )								\
	{											\
		DL_PF( "~*Not Implemented %s", msg );	\
		DAEDALUS_DL_ERROR( "%s: %08x %08x", (msg), command.inst.cmd0, command.inst.cmd1 );				\
		shown = true;							\
	}											\
}
#else
#define DL_UNIMPLEMENTED_ERROR( msg )
#endif

//*****************************************************************************
//
//*****************************************************************************
#if defined(DAEDALUS_DEBUG_DISPLAYLIST) || defined(DAEDALUS_ENABLE_PROFILING)
#define SetCommand( cmd, func, name )	gCustomInstruction[ cmd ] = func;	gCustomInstructionName[ cmd ] = name;
#else
#define SetCommand( cmd, func, name )	gCustomInstruction[ cmd ] = func;
#endif

#define MAX_DL_STACK_SIZE	32

#define N64COL_GETR( col )		(u8((col) >> 24))
#define N64COL_GETG( col )		(u8((col) >> 16))
#define N64COL_GETB( col )		(u8((col) >>  8))
#define N64COL_GETA( col )		(u8((col)      ))

#define N64COL_GETR_F( col )	(N64COL_GETR(col) * (1.0f/255.0f))
#define N64COL_GETG_F( col )	(N64COL_GETG(col) * (1.0f/255.0f))
#define N64COL_GETB_F( col )	(N64COL_GETB(col) * (1.0f/255.0f))
#define N64COL_GETA_F( col )	(N64COL_GETA(col) * (1.0f/255.0f))

// Mask down to 0x003FFFFF?
#define RDPSegAddr(seg) ( (gSegments[((seg)>>24)&0x0F]&0x00ffffff) + ((seg)&0x00FFFFFF) )
//*****************************************************************************
//
//*****************************************************************************

void RDP_MoveMemViewport(u32 address);
void MatrixFromN64FixedPoint( Matrix4x4 & mat, u32 address );
void DLParser_InitMicrocode( u32 code_base, u32 code_size, u32 data_base, u32 data_size );
void RDP_MoveMemLight(u32 light_idx, u32 address);

//////////////////////////////////////////////////////////
//////////////////////////////////////////////////////////
//                     GFX State                        //
//////////////////////////////////////////////////////////
//////////////////////////////////////////////////////////

struct N64Light
{
    u8 pad0, b, g, r;		// Colour
    u8 pad1, b2, g2, r2;	// Unused..
    s8 pad2, z, y, x;		// Direction
};
struct RDP_Scissor
{
	u32 left, top, right, bottom;
};

// The display list PC stack. Before this was an array of 10
// items, but this way we can nest as deeply as necessary.
struct DList
{
	u32 address[MAX_DL_STACK_SIZE];
	s32 limit;
};


// Used to keep track of when we're processing the first display list
static bool gFirstCall = true;

static u32				gSegments[16];
static RDP_Scissor		scissors;
static RDP_GeometryMode gGeometryMode;
static DList			gDlistStack;
static s32				gDlistStackPointer = -1;
static u32				gVertexStride	 = 0;
static u32				gFillColor		 = 0xFFFFFFFF;
static u32				gRDPHalf1		 = 0;
static u32				gLastUcodeBase   = 0;

       SImageDescriptor g_TI = { G_IM_FMT_RGBA, G_IM_SIZ_16b, 1, 0 };
static SImageDescriptor g_CI = { G_IM_FMT_RGBA, G_IM_SIZ_16b, 1, 0 };
static SImageDescriptor g_DI = { G_IM_FMT_RGBA, G_IM_SIZ_16b, 1, 0 };

const MicroCodeInstruction *gUcodeFunc = NULL;
MicroCodeInstruction gCustomInstruction[256];

#if defined(DAEDALUS_DEBUG_DISPLAYLIST) || defined(DAEDALUS_ENABLE_PROFILING)
const char ** gUcodeName = gNormalInstructionName[ 0 ];
const char * gCustomInstructionName[256];
#endif

bool					gFrameskipActive = false;

//*****************************************************************************
//
//*****************************************************************************
inline void FinishRDPJob()
{
	Memory_MI_SetRegisterBits(MI_INTR_REG, MI_INTR_DP);
	gCPUState.AddJob(CPU_CHECK_INTERRUPTS);
}

//*****************************************************************************
// Reads the next command from the display list, updates the PC.
//*****************************************************************************
inline void	DLParser_FetchNextCommand( MicroCodeCommand * p_command )
{
	// Current PC is the last value on the stack
	u32 pc = gDlistStack.address[gDlistStackPointer];
	*p_command = *(MicroCodeCommand*)(g_pu8RamBase + pc);

	gDlistStack.address[gDlistStackPointer]+= 8;

}

//*****************************************************************************
//
//*****************************************************************************
inline void DLParser_PopDL()
{
	DL_PF("    Returning from DisplayList: level=%d", gDlistStackPointer+1);
	DL_PF("    ############################################");
	DL_PF("    /\\ /\\ /\\ /\\ /\\ /\\ /\\ /\\ /\\ /\\ /\\ /\\ /\\ /\\ /\\");
	DL_PF(" ");

	gDlistStackPointer--;
}

#ifdef DAEDALUS_DEBUG_DISPLAYLIST
//////////////////////////////////////////////////////////
//                      Debug vars                      //
//////////////////////////////////////////////////////////
void DLParser_DumpVtxInfo(u32 address, u32 v0_idx, u32 num_verts);

u32			gNumDListsCulled;
u32			gNumVertices;
u32			gNumRectsClipped;
static u32	gCurrentInstructionCount = 0;			// Used for debugging display lists
u32			gTotalInstructionCount = 0;
static u32	gInstructionCountLimit = UNLIMITED_INSTRUCTION_COUNT;
#endif

//*****************************************************************************
//
//*****************************************************************************
u32 gRDPFrame		= 0;
u32 gAuxAddr		= (u32)g_pu8RamBase;

extern u32 uViWidth;
extern u32 uViHeight;
//*****************************************************************************
// Include ucode header files
//*****************************************************************************
#include "uCodes/Ucode_GBI0.h"
#include "uCodes/Ucode_GBI1.h"
#include "uCodes/Ucode_GBI2.h"
#include "uCodes/Ucode_DKR.h"
#include "uCodes/Ucode_GE.h"
#include "uCodes/Ucode_PD.h"
#include "uCodes/Ucode_Conker.h"
#include "uCodes/Ucode_LL.h"
#include "uCodes/Ucode_WRUS.h"
#include "uCodes/Ucode_SOTE.h"
#include "uCodes/Ucode_Sprite2D.h"
#include "uCodes/Ucode_S2DEX.h"

//////////////////////////////////////////////////////////
//////////////////////////////////////////////////////////
//                      Strings                         //
//////////////////////////////////////////////////////////
//////////////////////////////////////////////////////////

#ifdef DAEDALUS_DEBUG_DISPLAYLIST
static const char * const gFormatNames[8] = {"RGBA", "YUV", "CI", "IA", "I", "?1", "?2", "?3"};

static const char * const gSizeNames[4]   = {"4bpp", "8bpp", "16bpp", "32bpp"};
static const char * const gOnOffNames[2]  = {"Off", "On"};

//*****************************************************************************
//
//*****************************************************************************
void DLParser_DumpVtxInfo(u32 address, u32 v0_idx, u32 num_verts)
{
	if (gDisplayListFile != NULL)
	{
		s8 *pcSrc = (s8 *)(g_pu8RamBase + address);
		s16 *psSrc = (s16 *)(g_pu8RamBase + address);

		for ( u32 idx = v0_idx; idx < v0_idx + num_verts; idx++ )
		{
			f32 x = f32(psSrc[0^0x1]);
			f32 y = f32(psSrc[1^0x1]);
			f32 z = f32(psSrc[2^0x1]);

			u16 wFlags = u16(gRenderer->GetVtxFlags( idx )); //(u16)psSrc[3^0x1];

			u8 a = pcSrc[12^0x3];
			u8 b = pcSrc[13^0x3];
			u8 c = pcSrc[14^0x3];
			u8 d = pcSrc[15^0x3];

			s16 nTU = psSrc[4^0x1];
			s16 nTV = psSrc[5^0x1];

			f32 tu = f32(nTU) * (1.0f / 32.0f);
			f32 tv = f32(nTV) * (1.0f / 32.0f);

			const v4 & t = gRenderer->GetTransformedVtxPos( idx );
			const v4 & p = gRenderer->GetProjectedVtxPos( idx );

			psSrc += 8;			// Increase by 16 bytes
			pcSrc += 16;

			DL_PF("    #%02d Flags: 0x%04x Pos:{% 0.1f,% 0.1f,% 0.1f} Tex:{% 7.2f,% 7.2f} Extra: %02x %02x %02x %02x Tran:{% 0.3f,% 0.3f,% 0.3f,% 0.3f} Proj:{% 6f,% 6f,% 6f,% 6f}",
				idx, wFlags, x, y, z, tu, tv, a, b, c, d, t.x, t.y, t.z, t.w, p.x/p.w, p.y/p.w, p.z/p.w, p.w);
		}
	}
}

#endif
//*****************************************************************************
//
//*****************************************************************************
bool DLParser_Initialise()
{
	gFirstCall = true;

	// Reset scissor to default
	scissors.top = 0;
	scissors.left = 0;
	scissors.right = 320;
	scissors.bottom = 240;

	GBIMicrocode_Reset();

#ifdef DAEDALUS_FAST_TMEM
	//Clear pointers in TMEM block //Corn
	memset(gTlutLoadAddresses, 0, sizeof(gTlutLoadAddresses));
#endif
	return true;
}

//*****************************************************************************
//
//*****************************************************************************
void DLParser_Finalise()
{
}

//*************************************************************************************
// This is called from Microcode.cpp after a custom ucode has been detected and cached
// This function is only called once per custom ucode set
// Main resaon for this function is to save memory since custom ucodes share a common table
//	ucode:			custom ucode (ucode>= MAX_UCODE)
//	offset:			offset to normal ucode this custom ucode is based of ex GBI0
//*************************************************************************************
static void DLParser_SetCustom( u32 ucode, u32 offset )
{
	memcpy( &gCustomInstruction, &gNormalInstruction[offset], 1024 ); // sizeof(gNormalInstruction)/MAX_UCODE

#if defined(DAEDALUS_DEBUG_DISPLAYLIST) || defined(DAEDALUS_ENABLE_PROFILING)
	memcpy( gCustomInstructionName, gNormalInstructionName[ offset ], 1024 );
#endif

	// Start patching to create our custom ucode table ;)
	switch( ucode )
	{
		case GBI_GE:
			SetCommand( 0xb4, DLParser_RDPHalf1_GoldenEye, "G_RDPHalf1_GoldenEye" );
			break;
		case GBI_WR:
			SetCommand( 0x04, DLParser_GBI0_Vtx_WRUS, "G_Vtx_WRUS" );
			SetCommand( 0xb1, DLParser_Nothing,		  "G_Nothing" ); // Just in case
			break;
		case GBI_SE:
			SetCommand( 0x04, DLParser_GBI0_Vtx_SOTE, "G_Vtx_SOTE" );
			SetCommand( 0x06, DLParser_GBI0_DL_SOTE,  "G_DL_SOTE" );
			SetCommand( 0xfd, DLParser_SetTImg_SOTE,  "G_SetTImg_SOTE" );
			break;
		case GBI_LL:
			SetCommand( 0x80, DLParser_Last_Legion_0x80,	"G_Last_Legion_0x80" );
			SetCommand( 0x00, DLParser_Last_Legion_0x00,	"G_Last_Legion_0x00" );
			SetCommand( 0xe4, DLParser_TexRect_Last_Legion,	"G_TexRect_Last_Legion" );
			break;
		case GBI_PD:
			SetCommand( 0x04, DLParser_Vtx_PD,				"G_Vtx_PD" );
			SetCommand( 0x07, DLParser_Set_Vtx_CI_PD,		"G_Set_Vtx_CI_PD" );
			SetCommand( 0xb4, DLParser_RDPHalf1_GoldenEye,	"G_RDPHalf1_GoldenEye" );
			break;
		case GBI_DKR:
			SetCommand( 0x01, DLParser_Mtx_DKR,		 "G_Mtx_DKR" );
			SetCommand( 0x04, DLParser_GBI0_Vtx_DKR, "G_Vtx_DKR" );
			SetCommand( 0x05, DLParser_DMA_Tri_DKR,  "G_DMA_Tri_DKR" );
			SetCommand( 0x07, DLParser_DLInMem,		 "G_DLInMem" );
			SetCommand( 0xbc, DLParser_MoveWord_DKR, "G_MoveWord_DKR" );
			SetCommand( 0xbf, DLParser_Set_Addr_DKR, "G_Set_Addr_DKR" );
			SetCommand( 0xbb, DLParser_GBI1_Texture_DKR,"G_Texture_DKR" );
			break;
		case GBI_CONKER:
			SetCommand( 0x01, DLParser_Vtx_Conker,	"G_Vtx_Conker" );
			SetCommand( 0x05, DLParser_Tri1_Conker, "G_Tri1_Conker" );
			SetCommand( 0x06, DLParser_Tri2_Conker, "G_Tri2_Conker" );
			SetCommand( 0x10, DLParser_Tri4_Conker, "G_Tri4_Conker" );
			SetCommand( 0x11, DLParser_Tri4_Conker, "G_Tri4_Conker" );
			SetCommand( 0x12, DLParser_Tri4_Conker, "G_Tri4_Conker" );
			SetCommand( 0x13, DLParser_Tri4_Conker, "G_Tri4_Conker" );
			SetCommand( 0x14, DLParser_Tri4_Conker, "G_Tri4_Conker" );
			SetCommand( 0x15, DLParser_Tri4_Conker, "G_Tri4_Conker" );
			SetCommand( 0x16, DLParser_Tri4_Conker, "G_Tri4_Conker" );
			SetCommand( 0x17, DLParser_Tri4_Conker, "G_Tri4_Conker" );
			SetCommand( 0x18, DLParser_Tri4_Conker, "G_Tri4_Conker" );
			SetCommand( 0x19, DLParser_Tri4_Conker, "G_Tri4_Conker" );
			SetCommand( 0x1a, DLParser_Tri4_Conker, "G_Tri4_Conker" );
			SetCommand( 0x1b, DLParser_Tri4_Conker, "G_Tri4_Conker" );
			SetCommand( 0x1c, DLParser_Tri4_Conker, "G_Tri4_Conker" );
			SetCommand( 0x1d, DLParser_Tri4_Conker, "G_Tri4_Conker" );
			SetCommand( 0x1e, DLParser_Tri4_Conker, "G_Tri4_Conker" );
			SetCommand( 0x1f, DLParser_Tri4_Conker, "G_Tri4_Conker" );
			SetCommand( 0xdb, DLParser_MoveWord_Conker,  "G_MoveWord_Conker");
			SetCommand( 0xdc, DLParser_MoveMem_Conker,   "G_MoveMem_Conker" );
			break;
	}
}

//*****************************************************************************
//
//*****************************************************************************
void DLParser_InitMicrocode( u32 code_base, u32 code_size, u32 data_base, u32 data_size )
{
	u32 ucode = GBIMicrocode_DetectVersion( code_base, code_size, data_base, data_size, &DLParser_SetCustom );

	gVertexStride  = ucode_stride[ucode];
	gLastUcodeBase = code_base;
	gUcodeFunc	   = IS_CUSTOM_UCODE(ucode) ? gCustomInstruction : gNormalInstruction[ucode];

	// Used for fetching ucode names (Debug Only)
#if defined(DAEDALUS_DEBUG_DISPLAYLIST) || defined(DAEDALUS_ENABLE_PROFILING)
	gUcodeName = IS_CUSTOM_UCODE(ucode) ? gCustomInstructionName : gNormalInstructionName[ucode];
#endif
}

//*****************************************************************************
//
//*****************************************************************************
#ifdef DAEDALUS_ENABLE_PROFILING
SProfileItemHandle * gpProfileItemHandles[ 256 ];

#define PROFILE_DL_CMD( cmd )								\
	if(gpProfileItemHandles[ (cmd) ] == NULL)				\
	{														\
		gpProfileItemHandles[ (cmd) ] = new SProfileItemHandle( CProfiler::Get()->AddItem( gUcodeName[ cmd ] ));		\
	}														\
	CAutoProfile		_auto_profile( *gpProfileItemHandles[ (cmd) ] )

#else

#define PROFILE_DL_CMD( cmd )		do { } while(0)

#endif


//*****************************************************************************
//	Process the entire display list in one go
//*****************************************************************************
void	DLParser_ProcessDList()
{
	MicroCodeCommand command;

#ifdef DAEDALUS_DEBUG_DISPLAYLIST
	//Check if address is outside legal RDRAM
	u32			pc( gDlistStack.address[gDlistStackPointer] );

	if ( pc > MAX_RAM_ADDRESS )
	{
		DBGConsole_Msg(0, "Display list PC is out of range: 0x%08x", pc );
		return;
	}
#endif

	while(gDlistStackPointer >= 0)
	{
		DLParser_FetchNextCommand( &command );

		// Note: make sure have frame skip disabled for the dlist debugger to work
		//
#ifdef DAEDALUS_DEBUG_DISPLAYLIST
		//use the gInstructionName table for fecthing names.
		gCurrentInstructionCount++;
		DL_PF("[%05d] 0x%08x: %08x %08x %-10s", gCurrentInstructionCount, pc, command.inst.cmd0, command.inst.cmd1, gUcodeName[command.inst.cmd ]);

		PROFILE_DL_CMD( command.inst.cmd );

		gUcodeFunc[ command.inst.cmd ]( command );

		if( gInstructionCountLimit != UNLIMITED_INSTRUCTION_COUNT )
		{
			if( gCurrentInstructionCount >= gInstructionCountLimit )
			{
				return;
			}
		}
#else

		PROFILE_DL_CMD( command.inst.cmd );

		gUcodeFunc[ command.inst.cmd ]( command );
#endif
		// Check limit
		if (gDlistStack.limit >= 0)
		{
			if (--gDlistStack.limit < 0)
			{
				DL_PF("**EndDLInMem");
				gDlistStackPointer--;
				// limit is already reset to default -1 at this point
				//gDlistStack.limit = -1;
			}
		}

	}
}
//*****************************************************************************
//
//*****************************************************************************
void DLParser_Process()
{
	DAEDALUS_PROFILE( "DLParser_Process" );

	if ( !CGraphicsContext::Get()->IsInitialised() || !gRenderer )
	{
		return;
	}

	// Shut down the debug console when we start rendering
	// TODO: Clear the front/backbuffer the first time this function is called
	// to remove any stuff lingering on the screen.
	if(gFirstCall)
	{
#ifdef DAEDALUS_DEBUG_CONSOLE
		CDebugConsole::Get()->EnableConsole( false );
#endif
		CGraphicsContext::Get()->ClearAllSurfaces();

		gFirstCall = false;
	}

	// Update Screen only when something is drawn, otherwise several games ex Army Men will flash or shake.
	if( g_ROM.GameHacks != CHAMELEON_TWIST_2 ) gGraphicsPlugin->UpdateScreen();

	OSTask * pTask = (OSTask *)(g_pu8SpMemBase + 0x0FC0);
	u32 code_base = (u32)pTask->t.ucode & 0x1fffffff;
	u32 code_size = pTask->t.ucode_size;
	u32 data_base = (u32)pTask->t.ucode_data & 0x1fffffff;
	u32 data_size = pTask->t.ucode_data_size;
	u32 stack_size = pTask->t.dram_stack_size >> 6;

	if ( gLastUcodeBase != code_base )
	{
		DLParser_InitMicrocode( code_base, code_size, data_base, data_size );
	}

	//
	// Not sure what to init this with. We should probably read it from the dmem
	//
	gRDPOtherMode.L = 0x00500001;
	gRDPOtherMode.H = 0;

	gRDPFrame++;

	CTextureCache::Get()->PurgeOldTextures();

	// Initialise stack
	gDlistStackPointer=0;
	gDlistStack.address[gDlistStackPointer] = (u32)pTask->t.data_ptr;
	gDlistStack.limit = -1;

	gRDPStateManager.Reset();

#ifdef DAEDALUS_DEBUG_DISPLAYLIST
	gTotalInstructionCount = 0;
	gCurrentInstructionCount = 0;
	gNumDListsCulled = 0;
	gNumVertices = 0;
	gNumRectsClipped =0;
	//
	// Prepare to dump this displaylist, if necessary
	//
	DLDebug_HandleDumpDisplayList( pTask );
#endif

	DL_PF("DP: Firing up RDP!");

	if(!gFrameskipActive)
	{
		gRenderer->SetVIScales();
		gRenderer->ResetMatrices(stack_size);
		gRenderer->Reset();
		gRenderer->BeginScene();
		DLParser_ProcessDList();
		gRenderer->EndScene();
	}

	// Hack for Chameleon Twist 2, only works if screen is update at last
	//
	if( g_ROM.GameHacks == CHAMELEON_TWIST_2 ) gGraphicsPlugin->UpdateScreen();

	// Do this regardless!
	FinishRDPJob();

#ifdef DAEDALUS_DEBUG_DISPLAYLIST
	if (gDisplayListFile != NULL)
	{
		fclose( gDisplayListFile );
		gDisplayListFile = NULL;
	}

	gTotalInstructionCount = gCurrentInstructionCount;

#endif

#ifdef DAEDALUS_BATCH_TEST_ENABLED
	CBatchTestEventHandler * handler( BatchTest_GetHandler() );
	if( handler )
		handler->OnDisplayListComplete();
#endif
}

//*****************************************************************************
//
//*****************************************************************************
void MatrixFromN64FixedPoint( Matrix4x4 & mat, u32 address )
{

	DAEDALUS_ASSERT( address+64 < MAX_RAM_ADDRESS, "Mtx: Address invalid (0x%08x)", address);

	const f32 fRecip = 1.0f / 65536.0f;

	struct N64Imat
	{
		s16 h[4][4];
		u16 l[4][4];
	};
	const N64Imat *Imat = (N64Imat *)( g_pu8RamBase + address );

	s16 hi;
	u16 lo;
	s32 tmp;

	for (u32 i = 0; i < 4; i++)
	{
#if 1	// Crappy compiler.. reordring is to optimize the ASM // Corn
		hi = Imat->h[i][0 ^ U16H_TWIDDLE];
		lo = Imat->l[i][0 ^ U16H_TWIDDLE];
		tmp = ((hi << 16) | lo);
		hi = Imat->h[i][1 ^ U16H_TWIDDLE];
		mat.m[i][0] =  tmp * fRecip;

		lo = Imat->l[i][1 ^ U16H_TWIDDLE];
		tmp = ((hi << 16) | lo);
		hi = Imat->h[i][2 ^ U16H_TWIDDLE];
		mat.m[i][1] = tmp * fRecip;

		lo = Imat->l[i][2 ^ U16H_TWIDDLE];
		tmp = ((hi << 16) | lo);
		hi = Imat->h[i][3 ^ U16H_TWIDDLE];
		mat.m[i][2] = tmp * fRecip;

		lo = Imat->l[i][3 ^ U16H_TWIDDLE];
		tmp = ((hi << 16) | lo);
		mat.m[i][3] = tmp * fRecip;
#else

		hi = Imat->h[i][0 ^ U16H_TWIDDLE];
		lo = Imat->l[i][0 ^ U16H_TWIDDLE];
		mat.m[i][0] =  ((hi << 16) | lo) * fRecip;

		hi = Imat->h[i][1 ^ U16H_TWIDDLE];
		lo = Imat->l[i][1 ^ U16H_TWIDDLE];
		mat.m[i][1] = ((hi << 16) | lo) * fRecip;

		hi = Imat->h[i][2 ^ U16H_TWIDDLE];
		lo = Imat->l[i][2 ^ U16H_TWIDDLE];
		mat.m[i][2] = ((hi << 16) | lo) * fRecip;

		hi = Imat->h[i][3 ^ U16H_TWIDDLE];
		lo = Imat->l[i][3 ^ U16H_TWIDDLE];
		mat.m[i][3] = ((hi << 16) | lo) * fRecip;
#endif
	}
}

//*****************************************************************************
//
//*****************************************************************************
void RDP_MoveMemLight(u32 light_idx, u32 address)
{
	DAEDALUS_ASSERT( light_idx < 16, "Warning: invalid light # = %d", light_idx );

	N64Light *light = (N64Light*)(g_pu8RamBase + address);
	DL_PF("    Light[%d] RGB[%d, %d, %d] x[%d] y[%d] z[%d] %s direction",
		light_idx,
		light->r, light->g, light->b,
		light->x, light->y,	light->z,
		(light->x | light->y | light->z)? "Valid" : "Invalid"
		);

	//Light color
	gRenderer->SetLightCol( light_idx, light->r, light->g, light->b );

	//Direction
	if((light->x | light->y | light->z) != 0)
		gRenderer->SetLightDirection( light_idx, light->x, light->y, light->z );
}

//*****************************************************************************
//
//*****************************************************************************
//0x000b46b0: dc080008 800b46a0 G_GBI2_MOVEMEM
//    Type: 08 Len: 08 Off: 0000
//        Scale: 640 480 511 0 = 160,120
//        Trans: 640 480 511 0 = 160,120
//vscale is the scale applied to the normalized homogeneous coordinates after 4x4 projection transformation
//vtrans is the offset added to the scaled number

void RDP_MoveMemViewport(u32 address)
{

	DAEDALUS_ASSERT( address+16 < MAX_RAM_ADDRESS, "MoveMem Viewport, invalid memory" );

	s16 scale[2];
	s16 trans[2];

	// address is offset into RD_RAM of 8 x 16bits of data...
	scale[0] = *(s16 *)(g_pu8RamBase + ((address+(0*2))^0x2));
	scale[1] = *(s16 *)(g_pu8RamBase + ((address+(1*2))^0x2));
//	scale[2] = *(s16 *)(g_pu8RamBase + ((address+(2*2))^0x2));
//	scale[3] = *(s16 *)(g_pu8RamBase + ((address+(3*2))^0x2));

	trans[0] = *(s16 *)(g_pu8RamBase + ((address+(4*2))^0x2));
	trans[1] = *(s16 *)(g_pu8RamBase + ((address+(5*2))^0x2));
//	trans[2] = *(s16 *)(g_pu8RamBase + ((address+(6*2))^0x2));
//	trans[3] = *(s16 *)(g_pu8RamBase + ((address+(7*2))^0x2));

	// With D3D we had to ensure that the vp coords are positive, so
	// we truncated them to 0. This happens a lot, as things
	// seem to specify the scale as the screen w/2 h/2

	v2 vec_scale( scale[0] * 0.25f, scale[1] * 0.25f );
	v2 vec_trans( trans[0] * 0.25f, trans[1] * 0.25f );

	gRenderer->SetN64Viewport( vec_scale, vec_trans );

	DL_PF("    Scale: %d %d", scale[0], scale[1]);
	DL_PF("    Trans: %d %d", trans[0], trans[1]);
}

//*****************************************************************************
//
//*****************************************************************************
//Nintro64 uses Sprite2d
void DLParser_Nothing( MicroCodeCommand command )
{
	DAEDALUS_DL_ERROR( "RDP Command %08x Does not exist...", command.inst.cmd0 );

	// Terminate!
	//	DBGConsole_Msg(0, "Warning, DL cut short with unknown command: 0x%08x 0x%08x", command.inst.cmd0, command.inst.cmd1);
	DLParser_PopDL();

}

//*****************************************************************************
//
//*****************************************************************************
void DLParser_SetKeyGB( MicroCodeCommand command )
{
	DL_PF( "    SetKeyGB (Ignored)" );
}

//*****************************************************************************
//
//*****************************************************************************
void DLParser_SetKeyR( MicroCodeCommand command )
{
	DL_PF( "    SetKeyR (Ignored)" );
}

//*****************************************************************************
//
//*****************************************************************************
void DLParser_SetConvert( MicroCodeCommand command )
{
	DL_PF( "    SetConvert (Ignored)" );
}

//*****************************************************************************
//
//*****************************************************************************
void DLParser_SetPrimDepth( MicroCodeCommand command )
{
	DL_PF("    SetPrimDepth z[0x%04x] dz[0x%04x]",
		command.primdepth.z, command.primdepth.dz);

	gRenderer->SetPrimitiveDepth( command.primdepth.z );
}

//*****************************************************************************
//
//*****************************************************************************
void DLParser_RDPSetOtherMode( MicroCodeCommand command )
{
	DL_PF( "    RDPSetOtherMode: 0x%08x 0x%08x", command.inst.cmd0, command.inst.cmd1 );

	gRDPOtherMode.H = command.inst.cmd0;
	gRDPOtherMode.L = command.inst.cmd1;

#ifdef DAEDALUS_DEBUG_DISPLAYLIST
	DLDebug_DumpRDPOtherMode(gRDPOtherMode);
#endif
}

//*****************************************************************************
//
//*****************************************************************************
void DLParser_RDPLoadSync( MicroCodeCommand command )	{ /*DL_PF("    LoadSync: (Ignored)");*/ }
void DLParser_RDPPipeSync( MicroCodeCommand command )	{ /*DL_PF("    PipeSync: (Ignored)");*/ }
void DLParser_RDPTileSync( MicroCodeCommand command )	{ /*DL_PF("    TileSync: (Ignored)");*/ }

//*****************************************************************************
//
//*****************************************************************************
void DLParser_RDPFullSync( MicroCodeCommand command )
{
	// We now do this regardless
	// This is done after DLIST processing anyway
	//FinishRDPJob();

	/*DL_PF("    FullSync: (Generating Interrupt)");*/
}

//*****************************************************************************
//
//*****************************************************************************
void DLParser_SetScissor( MicroCodeCommand command )
{
	// The coords are all in 8:2 fixed point
	// Set up scissoring zone, we'll use it to scissor other stuff ex Texrect
	//
	scissors.left    = command.scissor.x0 >> 2;
	scissors.top     = command.scissor.y0 >> 2;
	scissors.right   = command.scissor.x1 >> 2;
	scissors.bottom  = command.scissor.y1 >> 2;

	// Hack to correct Super Bowling's right screen, left screen needs fb emulation
	if ( g_ROM.GameHacks == SUPER_BOWLING && g_CI.Address%0x100 != 0 )
	{
		// right half screen
		RDP_MoveMemViewport( RDPSegAddr(command.inst.cmd1) );
	}

	DL_PF("    x0=%d y0=%d x1=%d y1=%d mode=%d", scissors.left, scissors.top, scissors.right, scissors.bottom, command.scissor.mode);

	// Set the cliprect now...
	if ( scissors.left < scissors.right && scissors.top < scissors.bottom )
	{
		gRenderer->SetScissor( scissors.left, scissors.top, scissors.right, scissors.bottom );
	}
}
//*****************************************************************************
//
//*****************************************************************************
void DLParser_SetTile( MicroCodeCommand command )
{
	RDP_Tile tile;
	tile.cmd0 = command.inst.cmd0;
	tile.cmd1 = command.inst.cmd1;

	gRDPStateManager.SetTile( tile );

	DL_PF( "    Tile[%d] Format[%s/%s] Line[%d] TMEM[0x%03x] Palette[%d]", tile.tile_idx, gFormatNames[tile.format], gSizeNames[tile.size], tile.line, tile.tmem, tile.palette);
	DL_PF( "      S: Clamp[%s] Mirror[%s] Mask[0x%x] Shift[0x%x]", gOnOffNames[tile.clamp_s],gOnOffNames[tile.mirror_s], tile.mask_s, tile.shift_s );
	DL_PF( "      T: Clamp[%s] Mirror[%s] Mask[0x%x] Shift[0x%x]", gOnOffNames[tile.clamp_t],gOnOffNames[tile.mirror_t], tile.mask_t, tile.shift_t );
}

//*****************************************************************************
//
//*****************************************************************************
void DLParser_SetTileSize( MicroCodeCommand command )
{
	RDP_TileSize tile;
	tile.cmd0 = command.inst.cmd0;
	tile.cmd1 = command.inst.cmd1;

	DL_PF("    Tile[%d] (%d,%d) -> (%d,%d) [%d x %d]",
				tile.tile_idx, tile.left/4, tile.top/4,
		        tile.right/4, tile.bottom/4,
				((tile.right/4) - (tile.left/4)) + 1,
				((tile.bottom/4) - (tile.top/4)) + 1);

	gRDPStateManager.SetTileSize( tile );
}


//*****************************************************************************
//
//*****************************************************************************
void DLParser_SetTImg( MicroCodeCommand command )
{
	g_TI.Format		= command.img.fmt;
	g_TI.Size		= command.img.siz;
	g_TI.Width		= command.img.width + 1;
	g_TI.Address	= RDPSegAddr(command.img.addr);
	//g_TI.bpl		= g_TI.Width << g_TI.Size >> 1;

	DL_PF("    TImg Adr[0x%08x] Format[%s/%s] Width[%d] Pitch[%d] Bytes/line[%d]",
		g_TI.Address, gFormatNames[g_TI.Format], gSizeNames[g_TI.Size], g_TI.Width, g_TI.GetPitch(), g_TI.Width << g_TI.Size >> 1 );
}

//*****************************************************************************
//
//*****************************************************************************
void DLParser_LoadBlock( MicroCodeCommand command )
{
	gRDPStateManager.LoadBlock( command.loadtile );
}

//*****************************************************************************
//
//*****************************************************************************
void DLParser_LoadTile( MicroCodeCommand command )
{
	gRDPStateManager.LoadTile( command.loadtile );
}

//*****************************************************************************
//
//*****************************************************************************
void DLParser_LoadTLut( MicroCodeCommand command )
{
	gRDPStateManager.LoadTlut( command.loadtile );
}

//*****************************************************************************
//
//*****************************************************************************
void DLParser_TexRect( MicroCodeCommand command )
{
	MicroCodeCommand command2;
	MicroCodeCommand command3;

	//
	// Fetch the next two instructions
	//
	DLParser_FetchNextCommand( &command2 );
	DLParser_FetchNextCommand( &command3 );

	RDP_TexRect tex_rect;
	tex_rect.cmd0 = command.inst.cmd0;
	tex_rect.cmd1 = command.inst.cmd1;
	tex_rect.cmd2 = command2.inst.cmd1;
	tex_rect.cmd3 = command3.inst.cmd1;

	// NB: In FILL and COPY mode, rectangles are scissored to the nearest four pixel boundary.
	// This isn't currently handled, but I don't know of any games that depend on it.

	// Do compare with integers saves CPU //Corn
	u32	x0 = tex_rect.x0 >> 2;
	u32	y0 = tex_rect.y0 >> 2;
	u32	x1 = tex_rect.x1 >> 2;
	u32	y1 = tex_rect.y1 >> 2;

	// X for upper left corner should be less than X for lower right corner else skip rendering it
	// seems to happen in Rayman 2
	//if(x0 >= x1) return;

	// Removes offscreen texrect, also fixes several glitches like in John Romero's Daikatana
	if( x0 >= scissors.right || y0 >= scissors.bottom || x1 < scissors.left || y1 < scissors.top || g_CI.Format != G_IM_FMT_RGBA )
	{
#ifdef DAEDALUS_DEBUG_DISPLAYLIST
		++gNumRectsClipped;
#endif
		return;
	};

	//Not using floats here breaks GE 007 intro
	v2 d( tex_rect.dsdx / 1024.0f, tex_rect.dtdy / 1024.0f );
	v2 xy0( tex_rect.x0 / 4.0f, tex_rect.y0 / 4.0f );
	v2 xy1;
	v2 uv0( tex_rect.s / 32.0f, (tex_rect.dtdy < 0 ? tex_rect.t + 32 : tex_rect.t) / 32.0f );	//Fixes California Speed
	v2 uv1;

	DAEDALUS_DL_ASSERT(d.x > 0, "Negative or zero dsdx in TexRect (%f). UVs might be off.", d.x);

	//
	// In Fill/Copy mode the coordinates are inclusive (i.e. add 1.0f to the w/h)
	//
	switch ( gRDPOtherMode.cycle_type )
	{
		case CYCLE_COPY:
			d.x *= 0.25f;	// In copy mode 4 pixels are copied at once.
		case CYCLE_FILL:
			xy1.x = (tex_rect.x1 + 4) * 0.25f;
			xy1.y = (tex_rect.y1 + 4) * 0.25f;
			break;
		default:
			xy1.x = tex_rect.x1 * 0.25f;
			xy1.y = tex_rect.y1 * 0.25f;
			break;
	}

	uv1.x = uv0.x + d.x * ( xy1.x - xy0.x );
	uv1.y = uv0.y + d.y * ( xy1.y - xy0.y );

	DL_PF("    Screen(%.1f,%.1f) -> (%.1f,%.1f) Tile[%d]", xy0.x, xy0.y, xy1.x, xy1.y, tex_rect.tile_idx);
	DL_PF("    Tex:(%#5.3f,%#5.3f) -> (%#5.3f,%#5.3f) (DSDX:%#5f DTDY:%#5f)", uv0.x, uv0.y, uv1.x, uv1.y, d.x, d.y);

	gRenderer->TexRect( tex_rect.tile_idx, xy0, xy1, uv0, uv1 );
}

//*****************************************************************************
//
//*****************************************************************************
void DLParser_TexRectFlip( MicroCodeCommand command )
{
	MicroCodeCommand command2;
	MicroCodeCommand command3;

	//
	// Fetch the next two instructions
	//
	DLParser_FetchNextCommand( &command2 );
	DLParser_FetchNextCommand( &command3 );

	RDP_TexRect tex_rect;
	tex_rect.cmd0 = command.inst.cmd0;
	tex_rect.cmd1 = command.inst.cmd1;
	tex_rect.cmd2 = command2.inst.cmd1;
	tex_rect.cmd3 = command3.inst.cmd1;

	v2 d( tex_rect.dsdx / 1024.0f, tex_rect.dtdy / 1024.0f );
	v2 xy0( tex_rect.x0 / 4.0f, tex_rect.y0 / 4.0f );
	v2 xy1;
	v2 uv0( tex_rect.s / 32.0f, (tex_rect.dtdy < 0 ? tex_rect.t + 32 : tex_rect.t) / 32.0f );	//Fixes California Speed
	v2 uv1;

	DAEDALUS_DL_ASSERT(d.x > 0, "Negative or zero dsdx (%f) in TexRectFlip. UVs might be off.", d.x);
	DAEDALUS_DL_ASSERT(d.y > 0, "Negative or zero dtdy (%f) in TexRectFlip. UVs might be off.", d.y);

	//
	// In Fill/Copy mode the coordinates are inclusive (i.e. add 1.0f to the w/h)
	//
	switch ( gRDPOtherMode.cycle_type )
	{
		case CYCLE_COPY:
			d.x *= 0.25f;	// In copy mode 4 pixels are copied at once.
		case CYCLE_FILL:
			xy1.x = (tex_rect.x1 + 4) * 0.25f;
			xy1.y = (tex_rect.y1 + 4) * 0.25f;
			break;
		default:
			xy1.x = tex_rect.x1 * 0.25f;
			xy1.y = tex_rect.y1 * 0.25f;
			break;
	}

	uv1.x = uv0.x + d.x * ( xy1.y - xy0.y );		// Flip - use y
	uv1.y = uv0.y + d.y * ( xy1.x - xy0.x );		// Flip - use x

	DL_PF("    Screen(%.1f,%.1f) -> (%.1f,%.1f) Tile[%d]", xy0.x, xy0.y, xy1.x, xy1.y, tex_rect.tile_idx);
	DL_PF("    FLIPTex:(%#5.3f,%#5.3f) -> (%#5.3f,%#5.3f) (DSDX:%#5f DTDY:%#5f)", uv0.x, uv0.y, uv1.x, uv1.y, d.x, d.y);

	gRenderer->TexRectFlip( tex_rect.tile_idx, xy0, xy1, uv0, uv1 );
}

//*****************************************************************************
//
//*****************************************************************************
void DLParser_FillRect( MicroCodeCommand command )
{
	//
	// Removes annoying rect that appears in Conker and fillrects that cover screen in banjo tooie
	if( g_CI.Format != G_IM_FMT_RGBA )
	{
		DL_PF("    Ignoring Fillrect ");
		return;
	}

	// Note, in some modes, the right/bottom lines aren't drawn

	//Always clear Zbuffer if Depthbuffer is selected //Corn
	if (g_DI.Address == g_CI.Address)
	{
		CGraphicsContext::Get()->ClearZBuffer();
		DL_PF("    Clearing ZBuffer");
		return;
	}

	// TODO - Check colour image format to work out how this should be decoded!
	c32		colour;

	// Clear the screen if large rectangle?
	// This seems to mess up with the Zelda game select screen
	// For some reason it draws a large rect over the entire
	// display, right at the end of the dlist. It sets the primitive
	// colour just before, so maybe I'm missing something??
	// Problem was that we can only clear screen in fill mode

	u32 cycle_mode = gRDPOtherMode.cycle_type;

	if ( cycle_mode == CYCLE_FILL )
	{
		if(g_CI.Size == G_IM_SIZ_16b)
		{
			N64Pf5551	c( (u16)gFillColor );
			colour = ConvertPixelFormat< c32, N64Pf5551 >( c );
		}
		else
		{
			N64Pf8888	c( (u32)gFillColor );
			colour = ConvertPixelFormat< c32, N64Pf8888 >( c );
		}

		const u32 clear_screen_x = ( (command.fillrect.x1 - command.fillrect.x0) );
		const u32 clear_screen_y = ( (command.fillrect.y1 - command.fillrect.y0) );

		// Clear color buffer (screen clear)
		if( uViWidth == clear_screen_x && uViHeight == clear_screen_y )
		{
			CGraphicsContext::Get()->ClearColBuffer( colour );
			DL_PF("    Clearing Colour Buffer");
			return;
		}
	}
	else
	{
		// Should we use Prim or Blend colour? Doesn't work well see Mk64 transition before a race
		colour = c32(0);
	}

	DL_PF("    Filling Rectangle (%d,%d)->(%d,%d)", command.fillrect.x0, command.fillrect.y0, command.fillrect.x1, command.fillrect.y1);

	v2 xy0( command.fillrect.x0, command.fillrect.y0 );
	v2 xy1( command.fillrect.x1, command.fillrect.y1 );

	//
	// In Fill/Copy mode the coordinates are inclusive (i.e. add 1.0f to the w/h)
	//
	if ( cycle_mode >= CYCLE_COPY )
	{
		xy1.x += 1.0f;
		xy1.y += 1.0f;
	}

	// TODO - In 1/2cycle mode, skip bottom/right edges!?
	// This is done in BaseRenderer.

	gRenderer->FillRect( xy0, xy1, colour.GetColour() );
}

//*****************************************************************************
//
//*****************************************************************************
void DLParser_SetZImg( MicroCodeCommand command )
{
	DL_PF("    ZImg Adr[0x%08x]", RDPSegAddr(command.inst.cmd1));

	g_DI.Address = RDPSegAddr(command.inst.cmd1);
}

//*****************************************************************************
//
//*****************************************************************************
void DLParser_SetCImg( MicroCodeCommand command )
{
	g_CI.Format = command.img.fmt;
	g_CI.Size   = command.img.siz;
	g_CI.Width  = command.img.width + 1;
	g_CI.Address = RDPSegAddr(command.img.addr);
	//g_CI.Bpl		= g_CI.Width << g_CI.Size >> 1;

	DL_PF("    CImg Adr[0x%08x] Format[%s] Size[%s] Width[%d]", RDPSegAddr(command.inst.cmd1), gFormatNames[ g_CI.Format ], gSizeNames[ g_CI.Size ], g_CI.Width);
}

//*****************************************************************************
//
//*****************************************************************************
void DLParser_SetCombine( MicroCodeCommand command )
{
	//Swap the endian
	REG64 Mux;
	Mux._u32_0 = command.inst.cmd1;
	Mux._u32_1 = command.inst.arg0;

	gRenderer->SetMux( Mux._u64 );

#ifdef DAEDALUS_DEBUG_DISPLAYLIST
	if (gDisplayListFile != NULL)
	{
		DLDebug_DumpMux( Mux._u64 );
	}
#endif
}

//*****************************************************************************
//
//*****************************************************************************
void DLParser_SetFillColor( MicroCodeCommand command )
{
	gFillColor = command.inst.cmd1;

#ifdef DAEDALUS_DEBUG_DISPLAYLIST
	N64Pf5551	n64col( (u16)gFillColor );
	DL_PF( "    Color5551=0x%04x", n64col.Bits );
#endif
}

//*****************************************************************************
//
//*****************************************************************************
void DLParser_SetFogColor( MicroCodeCommand command )
{
	DL_PF("    RGBA: %d %d %d %d", command.color.r, command.color.g, command.color.b, command.color.a);

	c32	fog_colour( command.color.r, command.color.g, command.color.b, command.color.a );

	gRenderer->SetFogColour( fog_colour );
}

//*****************************************************************************
//
//*****************************************************************************
void DLParser_SetBlendColor( MicroCodeCommand command )
{
	DL_PF("    RGBA: %d %d %d %d", command.color.r, command.color.g, command.color.b, command.color.a);

	gRenderer->SetAlphaRef( command.color.a );
}

//*****************************************************************************
//
//*****************************************************************************
void DLParser_SetPrimColor( MicroCodeCommand command )
{
	DL_PF("    M:%d L:%d RGBA: %d %d %d %d", command.color.prim_min_level, command.color.prim_level, command.color.r, command.color.g, command.color.b, command.color.a);

	c32	prim_colour( command.color.r, command.color.g, command.color.b, command.color.a );

	gRenderer->SetPrimitiveColour( prim_colour );
}

//*****************************************************************************
//
//*****************************************************************************
void DLParser_SetEnvColor( MicroCodeCommand command )
{
	DL_PF("    RGBA: %d %d %d %d", command.color.r, command.color.g, command.color.b, command.color.a);

	c32	env_colour( command.color.r, command.color.g,command.color.b, command.color.a );

	gRenderer->SetEnvColour( env_colour );
}


#ifdef DAEDALUS_DEBUG_DISPLAYLIST
//*****************************************************************************
//
//*****************************************************************************
u32 DLParser_GetTotalInstructionCount()
{
	return gTotalInstructionCount;
}
#endif

#ifdef DAEDALUS_DEBUG_DISPLAYLIST
//*****************************************************************************
//
//*****************************************************************************
u32 DLParser_GetInstructionCountLimit()
{
	return gInstructionCountLimit;
}
#endif

#ifdef DAEDALUS_DEBUG_DISPLAYLIST
//*****************************************************************************
//
//*****************************************************************************
void DLParser_SetInstructionCountLimit( u32 limit )
{
	gInstructionCountLimit = limit;
}
#endif

//*****************************************************************************
//RSP TRI commands..
//In HLE emulation you NEVER see these commands !
//*****************************************************************************
void DLParser_TriRSP( MicroCodeCommand command ){ DL_PF("    RSP Tri: (Ignored)"); }


