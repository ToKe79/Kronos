#include "vdp1_compute.h"

#include <stdlib.h>
#include <stdio.h>
#include <math.h>
#include <string.h>
#include "vdp1.h"
#include "yui.h"

//#define VDP1CDEBUG
#ifdef VDP1CDEBUG
#define VDP1CPRINT printf
#else
#define VDP1CPRINT
#endif

#define NB_COARSE_RAST (NB_COARSE_RAST_X * NB_COARSE_RAST_Y)

extern vdp2rotationparameter_struct  Vdp1ParaA;

static int local_size_x = LOCAL_SIZE_X;
static int local_size_y = LOCAL_SIZE_Y;

static int tex_width;
static int tex_height;
static float tex_ratiow;
static float tex_ratioh;
static int struct_size;

static int work_groups_x;
static int work_groups_y;

static vdp1cmd_struct* cmdVdp1List;
static int* cmdVdp1;
static int* nbCmd;
static int* hasDrawingCmd;
static int nbCmdToProcess = 0;

static int cmdRam_update_start[2] = {0x0};
static int cmdRam_update_end[2] = {0x80000};

static int generateComputeBuffer(int w, int h);

static GLuint compute_tex[2] = {0};
static GLuint mesh_tex[2] = {0};
static GLuint ssbo_cmd_ = 0;
static GLuint ssbo_cmd_list_ = 0;
static GLuint ssbo_vdp1ram_[2] = {0};
static GLuint ssbo_nbcmd_ = 0;
static GLuint ssbo_vdp1access_ = 0;
static GLuint prg_vdp1[NB_PRG] = {0};

#ifdef VDP1RAM_CS_ASYNC
static YabEventQueue *cmdq[2] = {NULL};
static int vdp1_generate_run = 0;
#endif

static u32 write_fb[2][512*256];

static const GLchar * a_prg_vdp1[NB_PRG][5] = {
  //VDP1_MESH_STANDARD - BANDING
	{
		vdp1_start_f,
		vdp1_standard_mesh_f,
		vdp1_banding_f,
		vdp1_continue_no_mesh_f,
		vdp1_end_f
	},
	//VDP1_MESH_IMPROVED - BANDING
	{
		vdp1_start_f,
		vdp1_banding_f,
		vdp1_improved_mesh_f,
		vdp1_continue_mesh_f,
		vdp1_end_mesh_f
	},
	//VDP1_MESH_STANDARD - NO BANDING
	{
		vdp1_start_f,
		vdp1_standard_mesh_f,
		vdp1_no_banding_f,
		vdp1_continue_no_mesh_f,
		vdp1_end_f
	},
	//VDP1_MESH_IMPROVED- NO BANDING
	{
		vdp1_start_f,
		vdp1_no_banding_f,
		vdp1_improved_mesh_f,
		vdp1_continue_mesh_f,
		vdp1_end_mesh_f
	},
	//WRITE
	{
		vdp1_write_f,
		NULL,
		NULL,
		NULL,
		NULL
	},
	//READ
	{
		vdp1_read_f,
		NULL,
		NULL,
		NULL,
		NULL
	},
	//CLEAR
	{
		vdp1_clear_f,
		NULL,
		NULL,
		NULL,
		NULL
  },
	//CLEAR_MESH
	{
		vdp1_clear_mesh_f,
		NULL,
		NULL,
		NULL,
		NULL
	},
};

static int progMask = 0;

static int getProgramId() {
	if (_Ygl->meshmode == ORIGINAL_MESH){
	  if (_Ygl->bandingmode == ORIGINAL_BANDING)
    	return VDP1_MESH_STANDARD_BANDING;
		else
			return VDP1_MESH_STANDARD_NO_BANDING;
	}else{
		if (_Ygl->bandingmode == ORIGINAL_BANDING)
	  	return VDP1_MESH_IMPROVED_BANDING;
		else
			return VDP1_MESH_IMPROVED_NO_BANDING;
	}
}

int ErrorHandle(const char* name)
{
#ifdef VDP1CDEBUG
  GLenum   error_code = glGetError();
  if (error_code == GL_NO_ERROR) {
    return  1;
  }
  do {
    const char* msg = "";
    switch (error_code) {
    case GL_INVALID_ENUM:      msg = "INVALID_ENUM";      break;
    case GL_INVALID_VALUE:     msg = "INVALID_VALUE";     break;
    case GL_INVALID_OPERATION: msg = "INVALID_OPERATION"; break;
    case GL_OUT_OF_MEMORY:     msg = "OUT_OF_MEMORY";     break;
    case GL_INVALID_FRAMEBUFFER_OPERATION:  msg = "INVALID_FRAMEBUFFER_OPERATION"; break;
    default:  msg = "Unknown"; break;
    }
    YuiMsg("GLErrorLayer:ERROR:%04x'%s' %s\n", error_code, msg, name);
    error_code = glGetError();
  } while (error_code != GL_NO_ERROR);
  abort();
  return 0;
#else
  return 1;
#endif
}

static GLuint createProgram(int count, const GLchar** prg_strs) {
  GLint status;
	int exactCount = 0;
  GLuint result = glCreateShader(GL_COMPUTE_SHADER);

  for (int id = 0; id < count; id++) {
		if (prg_strs[id] != NULL) exactCount++;
		else break;
	}
  glShaderSource(result, exactCount, prg_strs, NULL);
  glCompileShader(result);
  glGetShaderiv(result, GL_COMPILE_STATUS, &status);

  if (status == GL_FALSE) {
    GLint length;
    glGetShaderiv(result, GL_INFO_LOG_LENGTH, &length);
    GLchar *info = (GLchar*)malloc(sizeof(GLchar) *length);
    glGetShaderInfoLog(result, length, NULL, info);
    YuiMsg("[COMPILE] %s\n", info);
    free(info);
    abort();
  }
  GLuint program = glCreateProgram();
  glAttachShader(program, result);
  glLinkProgram(program);
  glDetachShader(program, result);
  glGetProgramiv(program, GL_LINK_STATUS, &status);
  if (status == GL_FALSE) {
    GLint length;
    glGetProgramiv(program, GL_INFO_LOG_LENGTH, &length);
    GLchar *info = (GLchar*)malloc(sizeof(GLchar) *length);
    glGetProgramInfoLog(program, length, NULL, info);
    YuiMsg("[LINK] %s\n", info);
    free(info);
    abort();
  }
  return program;
}


static void regenerateMeshTex(int w, int h) {
	if (mesh_tex[0] != 0) {
		glDeleteTextures(2,&mesh_tex[0]);
	}
	glGenTextures(2, &mesh_tex[0]);
	glActiveTexture(GL_TEXTURE0);
	glBindTexture(GL_TEXTURE_2D, mesh_tex[0]);
	glBindBuffer(GL_PIXEL_UNPACK_BUFFER, 0);
	glTexStorage2D(GL_TEXTURE_2D, 1, GL_RGBA8, w, h);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
	glBindTexture(GL_TEXTURE_2D, mesh_tex[1]);
	glBindBuffer(GL_PIXEL_UNPACK_BUFFER, 0);
	glTexStorage2D(GL_TEXTURE_2D, 1, GL_RGBA8, w, h);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
}

static void vdp1_clear_mesh() {
	int progId = CLEAR_MESH;
	if (prg_vdp1[progId] == 0)
    prg_vdp1[progId] = createProgram(sizeof(a_prg_vdp1[progId]) / sizeof(char*), (const GLchar**)a_prg_vdp1[progId]);
  glUseProgram(prg_vdp1[progId]);
	glBindImageTexture(0, mesh_tex[0], 0, GL_FALSE, 0, GL_WRITE_ONLY, GL_RGBA8);
	glBindImageTexture(1, mesh_tex[1], 0, GL_FALSE, 0, GL_WRITE_ONLY, GL_RGBA8);
	glDispatchCompute(work_groups_x, work_groups_y, 1); //might be better to launch only the right number of workgroup
	glBindImageTexture(0, 0, 0, GL_FALSE, 0, GL_WRITE_ONLY, GL_RGBA8);
	glBindImageTexture(1, 0, 0, GL_FALSE, 0, GL_WRITE_ONLY, GL_RGBA8);
}

static int generateComputeBuffer(int w, int h) {
  if (compute_tex[0] != 0) {
    glDeleteTextures(2,&compute_tex[0]);
  }
	if (ssbo_vdp1ram_[0] != 0) {
    glDeleteBuffers(2, &ssbo_vdp1ram_[0]);
	}
	regenerateMeshTex(w, h);

	glGenBuffers(2, &ssbo_vdp1ram_[0]);
	glBindBuffer(GL_SHADER_STORAGE_BUFFER, ssbo_vdp1ram_[0]);
	glBufferData(GL_SHADER_STORAGE_BUFFER, 0x80000, NULL, GL_DYNAMIC_DRAW);
	glBindBuffer(GL_SHADER_STORAGE_BUFFER, ssbo_vdp1ram_[1]);
	glBufferData(GL_SHADER_STORAGE_BUFFER, 0x80000, NULL, GL_DYNAMIC_DRAW);

  if (ssbo_cmd_ != 0) {
    glDeleteBuffers(1, &ssbo_cmd_);
  }
	if (ssbo_cmd_list_ != 0) {
    glDeleteBuffers(1, &ssbo_cmd_list_);
  }
  glGenBuffers(1, &ssbo_cmd_);
  glBindBuffer(GL_SHADER_STORAGE_BUFFER, ssbo_cmd_);
  glBufferData(GL_SHADER_STORAGE_BUFFER, 4*QUEUE_SIZE*NB_COARSE_RAST, NULL, GL_DYNAMIC_DRAW);

	glGenBuffers(1, &ssbo_cmd_list_);
	glBindBuffer(GL_SHADER_STORAGE_BUFFER, ssbo_cmd_list_);
	glBufferData(GL_SHADER_STORAGE_BUFFER, struct_size*CMD_QUEUE_SIZE, NULL, GL_DYNAMIC_DRAW);

  if (ssbo_nbcmd_ != 0) {
    glDeleteBuffers(1, &ssbo_nbcmd_);
  }
  glGenBuffers(1, &ssbo_nbcmd_);
  glBindBuffer(GL_SHADER_STORAGE_BUFFER, ssbo_nbcmd_);
  glBufferData(GL_SHADER_STORAGE_BUFFER, NB_COARSE_RAST * sizeof(int),NULL,GL_DYNAMIC_DRAW);

	if (ssbo_vdp1access_ != 0) {
    glDeleteBuffers(1, &ssbo_vdp1access_);
	}

	glGenBuffers(1, &ssbo_vdp1access_);
	glBindBuffer(GL_SHADER_STORAGE_BUFFER, ssbo_vdp1access_);
	glBufferData(GL_SHADER_STORAGE_BUFFER, 512*256*4, NULL, GL_DYNAMIC_DRAW);

	float col[4] = {0.0};
	int limits[4] = {0, h, w, 0};
  glGenTextures(2, &compute_tex[0]);
  glActiveTexture(GL_TEXTURE0);
  glBindTexture(GL_TEXTURE_2D, compute_tex[0]);
  glBindBuffer(GL_PIXEL_UNPACK_BUFFER, 0);
  glTexStorage2D(GL_TEXTURE_2D, 1, GL_RGBA8, w, h);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
	vdp1_clear(0, col, limits);
  glBindTexture(GL_TEXTURE_2D, compute_tex[1]);
  glBindBuffer(GL_PIXEL_UNPACK_BUFFER, 0);
  glTexStorage2D(GL_TEXTURE_2D, 1, GL_RGBA8, w, h);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
	vdp1_clear(1, col, limits);
  return 0;
}

u8 cmdBuffer[2][0x80000];

void VIDCSGenerateBufferVdp1_sync(vdp1cmd_struct* cmd, int id) {
	int endcnt;
	u32 dot;
	int pos = (cmd->CMDSRCA * 8) & 0x7FFFF;
  u8 END = ((cmd->CMDPMOD & 0x80) != 0);
	u8* buf = &cmdBuffer[id][0];
	switch ((cmd->CMDPMOD >> 3) & 0x7) {
    case 0:
    case 1:
			for(int h=0; h < MAX(1, cmd->h); h++) {
				endcnt = 0;
				int width = (cmd->w==0)?1:cmd->w/2;
				for(int w=0; w < width; w++)
				{
					u32 addr1, addr2;
					dot = Vdp1RamReadByte(NULL, Vdp1Ram, pos);
					addr1 = ((dot>>4) * 2 + cmd->CMDCOLR * 8);
					addr2 = ((dot&0xF) * 2 + cmd->CMDCOLR * 8);
					if ((!END) && (endcnt >= 2)) {
          	dot |= 0xF0;
					}
					else if (((dot & 0xF0) == 0xF0) && (!END)) {
          	endcnt++;
        	} else {
							if (((cmd->CMDPMOD >> 3) & 0x7)==1) {
								//ColorLut
								u16 val = Vdp1RamReadWord(NULL, Vdp1Ram, addr1);
								if (cmdRam_update_start[id] > addr1) cmdRam_update_start[id] = addr1;
								if (cmdRam_update_end[id] < (addr1 + 2)) cmdRam_update_end[id] = addr1 + 2;
								T1WriteWord(buf, addr1, val);
							}
					}
					if ((!END) && (endcnt >= 2)) {
          	dot |= 0xF;
					}
					else if (((dot & 0xF) == 0xF) && (!END)) {
          	endcnt++;
					} else {
						if (((cmd->CMDPMOD >> 3) & 0x7)==1) {
							//ColorLut
							u16 val = Vdp1RamReadWord(NULL, Vdp1Ram, addr2);
							if (cmdRam_update_start[id] > addr2) cmdRam_update_start[id] = addr2;
							if (cmdRam_update_end[id] < (addr2 + 2)) cmdRam_update_end[id] = addr2 + 2;
							T1WriteWord(buf, addr2, val);
						}
        	}
					if (cmdRam_update_start[id] > pos) cmdRam_update_start[id] = pos;
					if (cmdRam_update_end[id] < (pos + 1)) cmdRam_update_end[id] = pos + 1;
					T1WriteByte(buf, pos, dot);
					pos += 1;
	    	}
			}
	    break;
	    case 2:
	    case 3:
	    case 4:
				for(int h=0; h < MAX(1, cmd->h); h++) {
					endcnt = 0;
					for(int w = 0; w < MAX(1, cmd->w); w++)
					{
						dot = Vdp1RamReadByte(NULL, Vdp1Ram, pos);
						if ((!END) && (endcnt >= 2)) {
							dot = 0xFF;
						}
						else if ((dot == 0xFF) && (!END)) {
							endcnt++;
						}
						if (cmdRam_update_start[id] > pos) cmdRam_update_start[id] = pos;
						if (cmdRam_update_end[id] < (pos + 1)) cmdRam_update_end[id] = pos + 1;
						T1WriteByte(buf, pos, dot);
						pos += 1;
		    	}
				}
	    break;
	    case 5:
			for(int h=0; h < cmd->h; h++) {
				endcnt = 0;
				for(int w = 0; w < cmd->w; w++)
	    	{
					u16 dot = Vdp1RamReadWord(NULL, Vdp1Ram, pos);
					if ((!END) && (endcnt >= 2)) {
						dot = 0x7FFF;
					}
					else if ((dot == 0x7FFF) && (!END)) {
						endcnt++;
					}
					if (cmdRam_update_start[id] > pos) cmdRam_update_start[id] = pos;
					if (cmdRam_update_end[id] < (pos + 2)) cmdRam_update_end[id] = pos + 2;
					T1WriteWord(buf, pos, dot);
					pos += 2;
	    	}
			}
	    break;
	  }
}
#ifdef VDP1RAM_CS_ASYNC
void* VIDCSGenerateBufferVdp1_async_0(void *p){
	while(vdp1_generate_run != 0){
		vdp1cmd_struct* cmd = (vdp1cmd_struct*)YabWaitEventQueue(cmdq[0]);
		if (cmd != NULL){
			VIDCSGenerateBufferVdp1_sync(cmd, 0);
			free(cmd);
		}
	}
	return NULL;
}
void* VIDCSGenerateBufferVdp1_async_1(void *p){
	while(vdp1_generate_run != 0){
		vdp1cmd_struct* cmd = (vdp1cmd_struct*)YabWaitEventQueue(cmdq[1]);
		if (cmd != NULL){
			VIDCSGenerateBufferVdp1_sync(cmd, 1);
			free(cmd);
		}
	}
	return NULL;
}

void VIDCSGenerateBufferVdp1(vdp1cmd_struct* cmd){
	vdp1cmd_struct* cmdToSent = (vdp1cmd_struct*)malloc(sizeof(vdp1cmd_struct));
	memcpy(cmdToSent, cmd, sizeof(vdp1cmd_struct));
	YabAddEventQueue(cmdq[_Ygl->drawframe], cmdToSent);
}
#else
void VIDCSGenerateBufferVdp1(vdp1cmd_struct* cmd){
	VIDCSGenerateBufferVdp1_sync(cmd, _Ygl->drawframe);
}
#endif

int vdp1_add(vdp1cmd_struct* cmd, int clipcmd) {
	int minx = 1024;
	int miny = 1024;
	int maxx = 0;
	int maxy = 0;

	int intersectX = -1;
	int intersectY = -1;
	int requireCompute = 0;

	if (_Ygl->wireframe_mode != 0) {
		int pos = (cmd->CMDSRCA * 8) & 0x7FFFF;
		switch(cmd->type ) {
			case DISTORTED:
			//By default use the central pixel code
			switch ((cmd->CMDPMOD >> 3) & 0x7) {
				case 0:
				case 1:
				  pos += (cmd->h/2) * cmd->w/2 + cmd->w/4;
					break;
				case 2:
				case 3:
				case 4:
					pos += (cmd->h/2) * cmd->w + cmd->w/2;
					break;
				case 5:
					pos += (cmd->h/2) * cmd->w*2 + cmd->w;
					break;
			}
			cmd->COLOR[0] = cmdBuffer[_Ygl->drawframe][pos];
			cmd->type = POLYLINE;
			break;
			case POLYGON:
				cmd->type = POLYLINE;
			break;
			case QUAD:
			case QUAD_POLY:
				if ((abs(cmd->CMDXA - cmd->CMDXB) <= ((2*_Ygl->rwidth)/3)) && (abs(cmd->CMDYA - cmd->CMDYD) <= ((_Ygl->rheight)/2)))
					cmd->type = POLYLINE;
			break;
			default:
				break;
		}
	}
	if (clipcmd == 0) {
		if (cmd->type != FB_WRITE) VIDCSGenerateBufferVdp1(cmd);
		else {
			requireCompute = 1;
		}
		if (_Ygl->meshmode != ORIGINAL_MESH) {
			//Hack for Improved MESH
			//Games like J.League Go Go Goal or Sailor Moon are using MSB shadow with VDP2 in RGB/Palette mode
			//In that case, the pixel is considered as RGB by the VDP2 displays it a black surface
			// To simualte a transparent shadow, on improved mesh, we force the shadow mode and the usage of mesh
			if ((cmd->CMDPMOD & 0x8000) && ((Vdp2Regs->SPCTL & 0x20)!=0)) {
				//MSB is set to be used but VDP2 do not use it. Consider as invalid and remove the MSB
				//Use shadow mode with Mesh to simulate the final effect
				cmd->CMDPMOD &= ~0x8007;
				cmd->CMDPMOD |= 0x101; //Use shadow mode and mesh then
			}
		}

	  float Ax = cmd->CMDXA;
		float Ay = cmd->CMDYA;
		float Bx = cmd->CMDXB;
		float By = cmd->CMDYB;
		float Cx = cmd->CMDXC;
		float Cy = cmd->CMDYC;
		float Dx = cmd->CMDXD;
		float Dy = cmd->CMDYD ;

	  minx = (Ax < Bx)?Ax:Bx;
	  miny = (Ay < By)?Ay:By;
	  maxx = (Ax > Bx)?Ax:Bx;
	  maxy = (Ay > By)?Ay:By;

	  minx = (minx < Cx)?minx:Cx;
	  minx = (minx < Dx)?minx:Dx;
	  miny = (miny < Cy)?miny:Cy;
	  miny = (miny < Dy)?miny:Dy;
	  maxx = (maxx > Cx)?maxx:Cx;
	  maxx = (maxx > Dx)?maxx:Dx;
	  maxy = (maxy > Cy)?maxy:Cy;
	  maxy = (maxy > Dy)?maxy:Dy;

	//Add a bounding box
	  cmd->B[0] = minx;
	  cmd->B[1] = (maxx);
	  cmd->B[2] = miny;
	  cmd->B[3] = (maxy);

		// YuiMsg("Bounding %d %d %d %d\n", minx, maxx, miny, maxy);

		progMask |= 1 << (cmd->CMDPMOD & 0x7u);
		if ((cmd->CMDPMOD & 0x8000u) == 0x8000u) progMask |= 0x100;
		if ((cmd->CMDPMOD & 0x40u) != 0) progMask |= 0x200; //SPD
		if ((cmd->CMDPMOD & 0x80u) != 0) progMask |= 0x400; //END
		progMask |= 0x1000 << ((cmd->CMDPMOD >> 3) & 0x7u);

	}
	memcpy(&cmdVdp1List[nbCmdToProcess], cmd, sizeof(vdp1cmd_struct));
  for (int i = 0; i<NB_COARSE_RAST_X; i++) {
    int blkx = i * (tex_width/NB_COARSE_RAST_X);
    for (int j = 0; j<NB_COARSE_RAST_Y; j++) {
      int blky = j*(tex_height/NB_COARSE_RAST_Y);
      if (!(blkx > maxx*_Ygl->vdp1wratio
        || (blkx + (tex_width/NB_COARSE_RAST_X)) < minx*_Ygl->vdp1wratio
        || (blky + (tex_height/NB_COARSE_RAST_Y)) < miny*_Ygl->vdp1hratio
        || blky > maxy*_Ygl->vdp1hratio)
			  || (clipcmd!=0)) {
					if (cmd->w == 0) cmd->w = 1;
					if (cmd->h == 0) cmd->h = 1;
					cmdVdp1[(i+j*NB_COARSE_RAST_X)*QUEUE_SIZE + nbCmd[i+j*NB_COARSE_RAST_X]] = nbCmdToProcess;
          nbCmd[i+j*NB_COARSE_RAST_X]++;
					if (clipcmd == 0) hasDrawingCmd[i+j*NB_COARSE_RAST_X] = 1;
					if (nbCmd[i+j*NB_COARSE_RAST_X] == QUEUE_SIZE) {
						requireCompute = 1;
					}
      }
    }
  }
	nbCmdToProcess++;
	if (nbCmdToProcess == CMD_QUEUE_SIZE) {
		requireCompute = 1;
	}
	if (requireCompute != 0){
		vdp1_compute();
		if (_Ygl->vdp1IsNotEmpty[_Ygl->drawframe] != -1) {
			vdp1_write();
			_Ygl->vdp1IsNotEmpty[_Ygl->drawframe] = -1;
		}
  }
  return 0;
}

void vdp1_clear(int id, float *col, int* lim) {
	int progId = CLEAR;
	int limits[4];
	memcpy(limits, lim, 4*sizeof(int));
	if (prg_vdp1[progId] == 0)
    prg_vdp1[progId] = createProgram(sizeof(a_prg_vdp1[progId]) / sizeof(char*), (const GLchar**)a_prg_vdp1[progId]);
	limits[0] = limits[0]*_Ygl->vdp1width/512;
	limits[1] = limits[1]*_Ygl->vdp1height/256;
	limits[2] = limits[2]*_Ygl->vdp1width/512;
	limits[3] = limits[3]*_Ygl->vdp1height/256;
  glUseProgram(prg_vdp1[progId]);
	glBindImageTexture(0, get_vdp1_tex(id), 0, GL_FALSE, 0, GL_WRITE_ONLY, GL_RGBA8);
	glBindImageTexture(1, get_vdp1_mesh(id), 0, GL_FALSE, 0, GL_WRITE_ONLY, GL_RGBA8);
	glUniform4fv(2, 1, col);
	glUniform4iv(3, 1, limits);
	glDispatchCompute(work_groups_x, work_groups_y, 1); //might be better to launch only the right number of workgroup
	glBindImageTexture(0, 0, 0, GL_FALSE, 0, GL_WRITE_ONLY, GL_RGBA8);
	glBindImageTexture(1, 0, 0, GL_FALSE, 0, GL_WRITE_ONLY, GL_RGBA8);
}

void vdp1_write() {
	int progId = WRITE;
	float wratio = 1.0f/_Ygl->vdp1wratio;
	float hratio = 1.0f/_Ygl->vdp1hratio;

	if (prg_vdp1[progId] == 0) {
    prg_vdp1[progId] = createProgram(sizeof(a_prg_vdp1[progId]) / sizeof(char*), (const GLchar**)a_prg_vdp1[progId]);
	}
  glUseProgram(prg_vdp1[progId]);

	glBindImageTexture(0, get_vdp1_tex(_Ygl->drawframe), 0, GL_FALSE, 0, GL_WRITE_ONLY, GL_RGBA8);
	glBindImageTexture(1, _Ygl->vdp1AccessTex[_Ygl->drawframe], 0, GL_FALSE, 0, GL_READ_ONLY, GL_RGBA8);
	glUniform2f(2, wratio, hratio);

	glDispatchCompute(work_groups_x, work_groups_y, 1); //might be better to launch only the right number of workgroup
	glBindImageTexture(0, 0, 0, GL_FALSE, 0, GL_WRITE_ONLY, GL_RGBA8);
	glBindImageTexture(1, 0, 0, GL_FALSE, 0, GL_READ_ONLY, GL_RGBA8);
}

u32* vdp1_read(int frame) {
	int progId = READ;
	float wratio = 1.0f/_Ygl->vdp1wratio;
	float hratio = 1.0f/_Ygl->vdp1hratio;
	if (prg_vdp1[progId] == 0)
    prg_vdp1[progId] = createProgram(sizeof(a_prg_vdp1[progId]) / sizeof(char*), (const GLchar**)a_prg_vdp1[progId]);
  glUseProgram(prg_vdp1[progId]);

	glBindImageTexture(0, get_vdp1_tex(frame), 0, GL_FALSE, 0, GL_READ_ONLY, GL_RGBA8);
	glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 1, ssbo_vdp1access_);
	glUniform2f(2, wratio, hratio);

	glDispatchCompute(work_groups_x, work_groups_y, 1); //might be better to launch only the right number of workgroup

  glMemoryBarrier(GL_BUFFER_UPDATE_BARRIER_BIT);

	glBindImageTexture(0, 0, 0, GL_FALSE, 0, GL_READ_ONLY, GL_RGBA8);

#ifdef _OGL3_
	glGetBufferSubData(GL_SHADER_STORAGE_BUFFER, 0x0, 512*256*4, (void*)(&write_fb[frame][0]));
#endif

	glBindBuffer(GL_SHADER_STORAGE_BUFFER, 0);
	return &write_fb[frame][0];
}


void vdp1_compute_init(int width, int height, float ratiow, float ratioh)
{
  int am = sizeof(vdp1cmd_struct) % 16;
  tex_width = width;
  tex_height = height;
	tex_ratiow = ratiow;
	tex_ratioh = ratioh;
  struct_size = sizeof(vdp1cmd_struct);
  if (am != 0) {
    struct_size += 16 - am;
  }
	progMask = 0;
#ifdef VDP1RAM_CS_ASYNC
	if (vdp1_generate_run == 0) {
		vdp1_generate_run = 1;
		cmdq[0] = YabThreadCreateQueue(512);
		cmdq[1] = YabThreadCreateQueue(512);
		YabThreadStart(YAB_THREAD_CS_CMD_0, VIDCSGenerateBufferVdp1_async_0, NULL);
		YabThreadStart(YAB_THREAD_CS_CMD_1, VIDCSGenerateBufferVdp1_async_1, NULL);
	}
#endif
  work_groups_x = _Ygl->vdp1width / local_size_x;
  work_groups_y = _Ygl->vdp1height / local_size_y;
  generateComputeBuffer(_Ygl->vdp1width, _Ygl->vdp1height);
	if (nbCmd == NULL)
  	nbCmd = (int*)malloc(NB_COARSE_RAST *sizeof(int));
	if (hasDrawingCmd == NULL)
		hasDrawingCmd = (int*)malloc(NB_COARSE_RAST *sizeof(int));
  if (cmdVdp1List == NULL)
		cmdVdp1List = (vdp1cmd_struct*)malloc(CMD_QUEUE_SIZE*sizeof(vdp1cmd_struct));
	if (cmdVdp1 == NULL)
			cmdVdp1 = (int*)malloc(NB_COARSE_RAST*QUEUE_SIZE*sizeof(int));
  memset(nbCmd, 0, NB_COARSE_RAST*sizeof(int));
	nbCmdToProcess = 0;
	memset(hasDrawingCmd, 0, NB_COARSE_RAST*sizeof(int));
	memset(cmdVdp1, 0, NB_COARSE_RAST*QUEUE_SIZE*sizeof(int));
	memset(cmdVdp1List, 0, CMD_QUEUE_SIZE*sizeof(vdp1cmd_struct*));
	return;
}

void vdp1_wait_regenerate(void) {
	#ifdef VDP1RAM_CS_ASYNC
	while (YaGetQueueSize(cmdq[_Ygl->drawframe])!=0)
	{
		YabThreadYield();
	}
	#endif
}

void vdp1_setup(void) {
	if (ssbo_vdp1ram_[_Ygl->drawframe] == 0) return;
	vdp1_wait_regenerate();
	if (cmdRam_update_start[_Ygl->drawframe] < cmdRam_update_end[_Ygl->drawframe]) {
		glBindBuffer(GL_SHADER_STORAGE_BUFFER, ssbo_vdp1ram_[_Ygl->drawframe]);
		glBufferSubData(GL_SHADER_STORAGE_BUFFER, cmdRam_update_start[_Ygl->drawframe], (cmdRam_update_end[_Ygl->drawframe] - cmdRam_update_start[_Ygl->drawframe]), (void*)(&(cmdBuffer[_Ygl->drawframe])[cmdRam_update_start[_Ygl->drawframe]]));
		cmdRam_update_start[_Ygl->drawframe] = 0x80000;
		cmdRam_update_end[_Ygl->drawframe] = 0x0;
	}
}

int get_vdp1_tex(int id) {
	glMemoryBarrier(GL_SHADER_STORAGE_BARRIER_BIT);
	return compute_tex[id];
}

int get_vdp1_mesh(int id) {
	return mesh_tex[id];
}
static int oldProg = -1;
void vdp1_compute() {
  GLuint error;
	int progId = getProgramId();
	int needRender = 0;
  for (int i = 0; i < NB_COARSE_RAST; i++) {
    if (hasDrawingCmd[i] == 0) nbCmd[i] = 0;
    if (nbCmd[i] != 0) {
			needRender = 1;
			break;
		}
  }
  if (needRender == 0) {
		nbCmdToProcess = 0;
		VDP1CPRINT("No cmd to draw\n");
		return;
	}

	if (prg_vdp1[progId] == 0)
	prg_vdp1[progId] = createProgram(sizeof(a_prg_vdp1[progId]) / sizeof(char*), (const GLchar**)a_prg_vdp1[progId]);

// YuiMsg("Use program 0x%x\n", progMask);

	glUseProgram(prg_vdp1[progId]);

	VDP1CPRINT("Draw VDP1 on %d\n", _Ygl->drawframe);
	if ((oldProg != -1) && (oldProg != progId)) {
		//CleanUp mesh texture
		vdp1_clear_mesh();
	}
	oldProg = progId;

	glBindBuffer(GL_SHADER_STORAGE_BUFFER, ssbo_cmd_);
	for (int i = 0; i < NB_COARSE_RAST; i++) {
		if (nbCmd[i] != 0) {
			glBufferSubData(GL_SHADER_STORAGE_BUFFER, 4*i*QUEUE_SIZE, nbCmd[i]*sizeof(int), (void*)&cmdVdp1[QUEUE_SIZE*i]);
		}
	}
	glBindBuffer(GL_SHADER_STORAGE_BUFFER, ssbo_cmd_list_);
	glBufferSubData(GL_SHADER_STORAGE_BUFFER, 0, nbCmdToProcess*sizeof(vdp1cmd_struct), (void*)&cmdVdp1List[0]);

	glBindBuffer(GL_SHADER_STORAGE_BUFFER, ssbo_nbcmd_);
  glBufferSubData(GL_SHADER_STORAGE_BUFFER, 0, sizeof(int)*NB_COARSE_RAST, (void*)nbCmd);

	glBindImageTexture(0, compute_tex[_Ygl->drawframe], 0, GL_FALSE, 0, GL_READ_WRITE, GL_RGBA8);
	glBindImageTexture(1, mesh_tex[_Ygl->drawframe], 0, GL_FALSE, 0, GL_WRITE_ONLY, GL_RGBA8);
	glBindImageTexture(2, _Ygl->vdp1AccessTex[_Ygl->drawframe], 0, GL_FALSE, 0, GL_READ_ONLY, GL_RGBA8);

	glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 3, ssbo_vdp1ram_[_Ygl->drawframe]);
  glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 4, ssbo_nbcmd_);
	glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 5, ssbo_cmd_);
	glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 6, ssbo_cmd_list_);
	glUniform2f(7, tex_ratiow, tex_ratioh);
	glUniform2i(8, Vdp1Regs->systemclipX2, Vdp1Regs->systemclipY2);
	glUniform4i(9, Vdp1Regs->userclipX1, Vdp1Regs->userclipY1, Vdp1Regs->userclipX2, Vdp1Regs->userclipY2);

	vdp1_setup();

  glDispatchCompute(work_groups_x, work_groups_y, 1); //might be better to launch only the right number of workgroup
  ErrorHandle("glDispatchCompute");
	progMask = 0;

	glBindImageTexture(0, 0, 0, GL_FALSE, 0, GL_WRITE_ONLY, GL_RGBA8);
	glBindImageTexture(1, 0, 0, GL_FALSE, 0, GL_WRITE_ONLY, GL_RGBA8);
	glBindImageTexture(2, 0, 0, GL_FALSE, 0, GL_READ_ONLY, GL_RGBA8);
  memset(nbCmd, 0, NB_COARSE_RAST*sizeof(int));
	nbCmdToProcess = 0;
	memset(hasDrawingCmd, 0, NB_COARSE_RAST*sizeof(int));
  glBindBuffer(GL_UNIFORM_BUFFER, 0);
	glBindTexture(GL_TEXTURE_2D, 0);

#ifdef TEST_FB_RW
	  vdp1_read();
	  vdp1_write();
#endif

  return;
}

void vdp1_compute_reset(void) {
	for(int i = 0; i<NB_PRG; i++) {
		if(prg_vdp1[i] != 0) {
			glDeleteProgram(prg_vdp1[i]);
			prg_vdp1[i] = 0;
		}
	}
}
