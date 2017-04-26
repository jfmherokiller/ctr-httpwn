#include <stdlib.h>
#include <malloc.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <unistd.h>
#include <dirent.h>
#include <errno.h>
#include <3ds.h>

#include "config.h"

#include "cmpblock_bin.h"

#include "builtin_rootca_der.h"

char regionids_table[7][4] = {//https://3dbrew.org/wiki/Nandrw/sys/SecureInfo_A
"JPN",
"USA",
"EUR",
"JPN", //"AUS"
"CHN",
"KOR",
"TWN"
};

extern Handle __httpc_servhandle;
extern u32 *__httpc_sharedmem_addr;

extern u32 ropvmem_size;
extern u32 httpheap_size;

vu32 *httpheap_sharedmem = NULL;
vu32 *ropvmem_sharedmem = NULL;

u8 *http_codebin_buf;
u32 *http_codebin_buf32;
u32 http_codebin_size;

Result init_hax_sharedmem(u32 *tmpbuf);
Result setuphaxx_httpheap_sharedmem(targeturlctx *first_targeturlctx);

Result loadcodebin(u64 programid, FS_MediaType mediatype, u8 **codebin_buf, u32 *codebin_size);

void displaymessage_waitbutton()
{
	printf("\nPress the A button to continue.\n");
	while(1)
	{
		gspWaitForVBlank();
		hidScanInput();
		if(hidKeysDown() & KEY_A)break;
	}
}

Result display_config_message(configctx *config, const char *str)
{
	if(config->message[0])
	{
		printf("%s\n%s\n\n", str, config->message);

		if(config->message_prompt)
		{
			printf("Press A to continue, B to abort.\n");

			while(1)
			{
				gspWaitForVBlank();
				hidScanInput();
				if(hidKeysDown() & KEY_A)break;
				if(hidKeysDown() & KEY_B)return 1;
			}
		}
	}

	return 0;
}

Result _HTTPC_CloseContext(Handle handle, Handle contextHandle, Handle *httpheap_sharedmem_handle, Handle *ropvmem_sharedmem_handle, Handle *httpc_sslc_handle)
{
	u32* cmdbuf=getThreadCommandBuffer();

	cmdbuf[0]=IPC_MakeHeader(0x3,1,0); // 0x30040
	cmdbuf[1]=contextHandle;
	
	Result ret=0;
	if(R_FAILED(ret=svcSendSyncRequest(handle)))return ret;

	if(cmdbuf[1]==0)
	{
		if(cmdbuf[0]!=0x00030045)return -1;//The ROP is supposed to return a custom cmdreply.
		if(cmdbuf[2]!=(0x10 | ((0x2-1)<<26)) || cmdbuf[5]!=0x0)return -1;

		if(httpheap_sharedmem_handle)*httpheap_sharedmem_handle = cmdbuf[3];
		if(ropvmem_sharedmem_handle)*ropvmem_sharedmem_handle = cmdbuf[4];
		if(httpc_sslc_handle)*httpc_sslc_handle = cmdbuf[6];
	}

	return cmdbuf[1];
}

Result _httpcCloseContext(httpcContext *context, Handle *httpheap_sharedmem_handle, Handle *ropvmem_sharedmem_handle, Handle *httpc_sslc_handle)
{
	Result ret=0;

	svcCloseHandle(context->servhandle);
	ret = _HTTPC_CloseContext(__httpc_servhandle, context->httphandle, httpheap_sharedmem_handle, ropvmem_sharedmem_handle, httpc_sslc_handle);

	return ret;
}

/*
The handleindex refers to what stored handle to process, must be 0-3(max 4 handles).
The in_handle should be 0 unless type0 is used.
Commands which don't match the 0x18010082 cmdhdr will be used with svcSendSyncRequest(storedhandle_index0).
Type0: Set the stored handle to the specified in_handle.
Type1: Return the stored handle with out_handle.
Type2: Close+clear the stored handle.
*/
Result _HTTPC_CustomCmd(Handle handle, u32 type, u32 handleindex, Handle in_handle, Handle *out_handle)
{
	u32* cmdbuf=getThreadCommandBuffer();

	cmdbuf[0]=IPC_MakeHeader(0x1801,2,2); // 0x18010082
	cmdbuf[1]=type;
	cmdbuf[2]=handleindex;
	cmdbuf[3]=IPC_Desc_SharedHandles(1);
	cmdbuf[4]=in_handle;

	Result ret=0;
	if(R_FAILED(ret=svcSendSyncRequest(handle)))return ret;
	ret = cmdbuf[1];

	if(ret==0)
	{
		if(type==1 && out_handle)
		{
			*out_handle = cmdbuf[3];
		}
	}

	return cmdbuf[1];
}

Result _httpcCustomCmd(httpcContext *context, u32 type, u32 handleindex, Handle in_handle, Handle *out_handle)
{
	return _HTTPC_CustomCmd(context->servhandle, type, handleindex, in_handle, out_handle);
}

//This searches physmem for the page which starts with the data stored in cmpblock_bin. The first byte in cmpblock is XORed with 0x01 to avoid detecting the cmpblock in physmem.
Result locate_sharedmem_linearaddr(u32 **linearaddr)
{
	u8 *tmpbuf;
	u32 chunksize = 0x100000;
	u32 linearpos, bufpos, size;
	u32 i;
	u32 xorval;
	int found = 0;

	*linearaddr = NULL;

	tmpbuf = linearAlloc(chunksize);
	if(tmpbuf==NULL)
	{
		printf("Failed to allocate mem for tmpbuf.\n");
		return -1;
	}

	size = osGetMemRegionSize(MEMREGION_APPLICATION);

	for(linearpos=0; linearpos<size; linearpos+= chunksize)
	{
		*linearaddr = (u32*)(0x30000000+linearpos);

		memset(tmpbuf, 0, chunksize);
		GSPGPU_FlushDataCache(tmpbuf, chunksize);

		GX_TextureCopy(*linearaddr, 0, (u32*)tmpbuf, 0, chunksize, 0x8);
		gspWaitForPPF();

		for(bufpos=0; bufpos<chunksize; bufpos+= 0x1000)
		{
			found = 1;

			for(i=0; i<cmpblock_bin_size; i++)
			{
				xorval = 0;
				if(i==0)xorval = 1;

				if(tmpbuf[bufpos + i] != (cmpblock_bin[i] ^ xorval))
				{
					found = 0;
					break;
				}
			}

			if(found)
			{
				*linearaddr = (u32*)(0x30000000+linearpos+bufpos);
				break;
			}
		}
		if(found)break;
	}

	linearFree(tmpbuf);

	if(!found)return -1;

	return 0;
}

Result writehax_sharedmem_physmem(u32 *linearaddr)
{
	Result ret=0;
	u32 chunksize = 0x1000;
	u32 *tmpbuf;

	//Allocate memory for the sharedmem page, then copy the sharedmem physmem data into the buf.

	tmpbuf = linearAlloc(chunksize);
	if(tmpbuf==NULL)
	{
		printf("Failed to allocate mem for tmpbuf.\n");
		return -1;
	}

	memset(tmpbuf, 0, chunksize);
	GSPGPU_FlushDataCache(tmpbuf, chunksize);

	GX_TextureCopy(linearaddr, 0, tmpbuf, 0, chunksize, 0x8);
	gspWaitForPPF();

	ret = init_hax_sharedmem(tmpbuf);
	if(ret)
	{
		linearFree(tmpbuf);
		return ret;
	}

	//Flush dcache for the modified sharedmem, then copy the data back into the sharedmem physmem.
	GSPGPU_FlushDataCache(tmpbuf, chunksize);

	GX_TextureCopy(tmpbuf, 0, linearaddr, 0, chunksize, 0x8);
	gspWaitForPPF();

	linearFree(tmpbuf);

	return 0;
}

Result setuphax_http_sslc(Handle httpc_sslc_handle, u8 *cert, u32 certsize)
{
	Result ret=0;
	u32 RootCertChain_contexthandle;

	ret = sslcInit(httpc_sslc_handle);
	if(R_FAILED(ret))
	{
		printf("Failed to initialize sslc: 0x%08x.\n", (unsigned int)ret);
		return ret;
	}

	//RootCertChain_contexthandle 0x1/0x2 are the first/second NIM-sysmodule RootCertChain. 0x3 is the ACT-sysmodule RootCertChain, which isn't used here.
	for(RootCertChain_contexthandle=0x1; RootCertChain_contexthandle<0x4; RootCertChain_contexthandle++)
	{
		ret = sslcAddTrustedRootCA(RootCertChain_contexthandle, cert, certsize, NULL);
		if(R_FAILED(ret))break;
	}

	sslcExit();

	return ret;
}

Result test_customcmdhandler(httpcContext *context)
{
	Result ret=0, ret2=0;
	psRSAContext rsactx;
	Handle tmphandle=0;
	u32 tmpval;
	u8 signature[0x100];
	u8 hash[0x20];
	u32 cryptblock[0x10>>2];
	u8 iv[0x10];

	memset(&rsactx, 0, sizeof(rsactx));
	memset(signature, 0, sizeof(signature));
	memset(hash, 0, sizeof(hash));
	rsactx.rsa_bitsize = 0x100<<3;

	memset(cryptblock, 0, sizeof(cryptblock));
	memset(iv, 0, sizeof(iv));

	ret = _httpcCustomCmd(context, ~0, 0, 0, NULL);//Run the customcmd handler with an invalid type-value so that the static-buffer is setup, for use with PS_VerifyRsaSha256.
	if(ret!=0)
	{
		printf("The initial _httpcCustomCmd() returned 0x%08x.\n", (unsigned int)ret);
		return ret;
	}

	ret = svcDuplicateHandle(&tmphandle, context->servhandle);//The context servhandle needs duplicated before using with psInitHandle, since psExit will close it.
	if(R_FAILED(ret))
	{
		printf("svcDuplicateHandle failed: 0x%08x.\n", (unsigned int)ret);
		return ret;
	}

	ret = psInitHandle(tmphandle);
	if(R_FAILED(ret))
	{
		printf("psInitHandle failed: 0x%08x.\n", (unsigned int)ret);
		return ret;
	}

	ret = PS_VerifyRsaSha256(hash, &rsactx, signature);
	if(ret!=0)
	{
		printf("Custom PS_VerifyRsaSha256 failed: 0x%08x.\n", (unsigned int)ret);
	}

	psExit();

	if(ret!=0)return ret;

	ret = psInit();
	if(R_SUCCEEDED(ret))
	{
		printf("Testing with the actual ps:ps service...\n");

		ret = PS_VerifyRsaSha256(hash, &rsactx, signature);
		printf("Normal PS_VerifyRsaSha256 returned 0x%08x.\n", (unsigned int)ret);
		ret = 0;

		ret = _httpcCustomCmd(context, 0, 0, psGetSessionHandle(), NULL);
		if(R_FAILED(ret))
		{
			printf("Failed to send the ps:ps handle: 0x%08x.\n", (unsigned int)ret);
		}

		if(ret==0)
		{
			tmphandle = 0;
			ret = _httpcCustomCmd(context, 1, 0, 0, &tmphandle);
			if(R_FAILED(ret))
			{
				printf("Failed to get the handle: 0x%08x.\n", (unsigned int)ret);
			}
			else
			{
				if(tmphandle==0)
				{
					printf("Invalid output handle: 0x%08x.\n", (unsigned int)tmphandle);
					ret = -2;
				}
			}

			if(ret==0)
			{
				psExit();

				tmphandle=0;
				ret = svcDuplicateHandle(&tmphandle, context->servhandle);//The context servhandle needs duplicated before using with psInitHandle, since psExit will close it.
				if(R_FAILED(ret))
				{
					printf("svcDuplicateHandle failed: 0x%08x.\n", (unsigned int)ret);
				}
				else
				{
					ret = psInitHandle(tmphandle);
					if(R_FAILED(ret))
					{
						printf("psInitHandle failed: 0x%08x.\n", (unsigned int)ret);
					}
					else
					{
						ret = PS_VerifyRsaSha256(hash, &rsactx, signature);
						printf("Custom+normal PS_VerifyRsaSha256 returned 0x%08x.\n", (unsigned int)ret);
					}

					if(ret==0)
					{
						tmpval = 0;
						ret = PS_GetDeviceId(&tmpval);//Verify that using a PS command via the custom-cmdhandler without special handling works fine.
						printf("PS_GetDeviceId returned 0x%08x, out=0x%08x.\n", (unsigned int)ret, (unsigned int)tmpval);
						if(ret==0 && tmpval==0)ret = -3;
					}

					if(ret==0)//Verify that PS_EncryptDecryptAes works fine since the custom-cmdhandler has additional handling for it.
					{
						ret = PS_EncryptDecryptAes(sizeof(cryptblock), (u8*)cryptblock, (u8*)cryptblock, PS_ALGORITHM_CTR_ENC, PS_KEYSLOT_0D, iv);
						printf("PS_EncryptDecryptAes returned 0x%08x, out=0x%08x.\n", (unsigned int)ret, (unsigned int)cryptblock[0]);
						if(ret==0 && tmpval==0)ret = -3;
					}
				}
			}

			ret2 = _httpcCustomCmd(context, 2, 0, 0, NULL);
			if(R_FAILED(ret2))
			{
				printf("Failed to close the stored handle: 0x%08x.\n", (unsigned int)ret2);
				if(ret==0)ret = ret2;
			}
		}

		psExit();
	}
	else//Ignore init failure since ps:ps normally isn't accessible.
	{
		ret = 0;
	}

	return ret;
}

Result test_boss(char *urlbase, httpcContext *httpcontext)
{
	Result ret=0;
	char *taskID = "task";
	u8 tmp0, tmp1;
	u32 tmp2;
	u32 NsDataId = 0x58584148;
	u8 tmpbuf[5] = {0};

	u8 region=0;

	bossContext ctx;

	Handle bosshandles[2];

	char url[256];

	//This tests BOSS(SpotPass) to verify that unsigned boss-container content can be used. This is also for running bosshaxx via the ctr-httpwn config for it. Even if bosshaxx wouldn't run at all, this would still work fine if sigchecks are already patched with the running system(like "cfw").

	printf("Testing BOSS...\n");

	ret = cfguInit();
	if(ret!=0)
	{
		printf("Failed to init cfg: 0x%08x.\n", (unsigned int)ret);
		return ret;
	}
	ret = CFGU_SecureInfoGetRegion(&region);
	if(ret!=0)
	{
		printf("Failed to get region from cfg: 0x%08x.\n", (unsigned int)ret);
		return ret;
	}
	if(region>=7)
	{
		printf("Region value from cfg is invalid: 0x%02x.\n", (unsigned int)region);
		ret = -9;
		return ret;
	}
	cfguExit();

	ret = bossInit(0, false);

	if(R_SUCCEEDED(ret))
	{
		printf("Deleting BOSS data...\n");

		ret = bossDeleteTask(taskID, 0);
		//printf("bossDeleteTask returned 0x%08x.\n", (unsigned int)ret);

		ret = bossDeleteNsData(NsDataId);
		//printf("bossDeleteNsData returned 0x%08x.\n", (unsigned int)ret);

		printf("Registering/starting the BOSS task...\n");

		memset(url, 0, sizeof(url));
		snprintf(url, sizeof(url)-1, "%s_%s", urlbase, regionids_table[region]);

		bossSetupContextDefault(&ctx, 60, url);

		ret = bossSendContextConfig(&ctx);
		if(R_FAILED(ret))printf("bossSendContextConfig returned 0x%08x.\n", (unsigned int)ret);

		if(R_SUCCEEDED(ret))
		{
			ret = bossRegisterTask(taskID, 0, 0);
			if(R_FAILED(ret))printf("bossRegisterTask returned 0x%08x.\n", (unsigned int)ret);

			if(R_SUCCEEDED(ret))
			{
				ret = bossStartTaskImmediate(taskID);
				if(R_FAILED(ret))printf("bossStartTaskImmediate returned 0x%08x.\n", (unsigned int)ret);
			}

			if(R_SUCCEEDED(ret))
			{
				printf("Waiting for the task to finish running...\n");

				while(1)
				{
					if(R_SUCCEEDED(ret))
					{
						ret = bossGetTaskState(taskID, 0, &tmp0, &tmp2, &tmp1);
						if(R_FAILED(ret))
						{
							printf("bossGetTaskState returned 0x%08x.\n", (unsigned int)ret);
							break;
						}
						if(R_SUCCEEDED(ret))printf("...\n");//printf("bossGetTaskState: tmp0=0x%x, tmp2=0x%x, tmp1=0x%x.\n", (unsigned int)tmp0, (unsigned int)tmp2, (unsigned int)tmp1);

						if(tmp0!=BOSSTASKSTATUS_STARTED)break;

						svcSleepThread(1000000000LL);//Delay 1s.
					}
				}
			}

			if(R_SUCCEEDED(ret) && tmp0==BOSSTASKSTATUS_ERROR)
			{
				printf("BOSS task failed. This usually indicates a network failure.\n");
				ret = -9;
			}

			if(R_SUCCEEDED(ret))
			{
				printf("Reading BOSS content...\n");

				tmp2 = 0;
				ret = bossReadNsData(NsDataId, 0, tmpbuf, sizeof(tmpbuf), &tmp2, NULL);
				if(R_FAILED(ret))printf("bossReadNsData returned 0x%08x, transfer_total=0x%x.\n", (unsigned int)ret, (unsigned int)tmp2);

				if(R_SUCCEEDED(ret) && tmp2!=sizeof(tmpbuf))ret = -10;
			}

			if(R_SUCCEEDED(ret))
			{
				if(strncmp((char*)tmpbuf, "Hello", 5))
				{
					printf("Invalid BOSS content data.\n");
					ret = -11;
				}
			}

			if(R_SUCCEEDED(ret))
			{
				printf("Deleting BOSS data...\n");

				ret = bossDeleteNsData(NsDataId);
				if(R_FAILED(ret))printf("bossDeleteNsData returned 0x%08x.\n", (unsigned int)ret);
			}

			if(R_SUCCEEDED(ret))
			{
				ret = bossDeleteTask(taskID, 0);
				if(R_FAILED(ret))printf("bossDeleteTask returned 0x%08x.\n", (unsigned int)ret);
			}

			if(R_SUCCEEDED(ret))
			{
				memset(bosshandles, 0, sizeof(bosshandles));

				ret = _httpcCustomCmd(httpcontext, 1, 0, 0, &bosshandles[0]);
				if(R_SUCCEEDED(ret))ret = _httpcCustomCmd(httpcontext, 1, 1, 0, &bosshandles[1]);

				if(R_FAILED(ret))
				{
					printf("Failed to get handles from the custom-cmdhandler: 0x%08x.\n", (unsigned int)ret);
				}
				else
				{
					if(bosshandles[0]==0 || bosshandles[1]==0)
					{
						printf("bosshaxx failed to run. This can be ignored since the BOSS-container content was loaded fine.\n");
					}
					else
					{
						printf("bosshaxx ran successfully.\n");

						svcCloseHandle(bosshandles[0]);
						svcCloseHandle(bosshandles[1]);
					}
				}
			}
		}

		bossExit();
	}
	else
	{
		printf("bossInit returned 0x%08x.\n", (unsigned int)ret);
		printf("*hax payload >=v2.8 is required.\n");
	}

	return ret;
}

Result http_haxx(char *requrl, u8 *cert, u32 certsize, targeturlctx *first_targeturlctx)
{
	Result ret=0;
	httpcContext context;
	u32 *linearaddr = NULL;
	Handle httpheap_sharedmem_handle=0;
	Handle ropvmem_sharedmem_handle=0;
	Handle httpc_sslc_handle = 0;
	u32 i;

	targeturlctx *boss_targeturlctx = NULL;

	ret = httpcOpenContext(&context, HTTPC_METHOD_POST, requrl, 1);
	if(ret!=0)return ret;

	ret = httpcAddPostDataAscii(&context, "form_name", "form_value");
	if(ret!=0)
	{
		httpcCloseContext(&context);
		return ret;
	}

	//Locate the physmem for the httpc sharedmem. With the current cmpblock, there can only be one POST struct that was ever written into sharedmem, with the name/value from above.
	printf("Searching for the httpc sharedmem in physmem...\n");
	ret = locate_sharedmem_linearaddr(&linearaddr);
	if(ret!=0)
	{
		printf("Failed to locate the sharedmem in physmem.\n");
		httpcCloseContext(&context);
		return ret;
	}

	printf("Writing the haxx to physmem...\n");
	ret = writehax_sharedmem_physmem(linearaddr);
	if(ret!=0)
	{
		printf("Failed to setup the haxx.\n");
		httpcCloseContext(&context);
		return ret;
	}

	printf("Triggering the haxx...\n");
	ret = _httpcCloseContext(&context, &httpheap_sharedmem_handle, &ropvmem_sharedmem_handle, &httpc_sslc_handle);
	if(R_FAILED(ret))
	{
		printf("httpcCloseContext returned 0x%08x.\n", (unsigned int)ret);
		return ret;
	}

	httpheap_sharedmem = (vu32*)mappableAlloc(httpheap_size);
	if(httpheap_sharedmem==NULL)
	{
		ret = -2;
		svcCloseHandle(httpheap_sharedmem_handle);
		svcCloseHandle(ropvmem_sharedmem_handle);
		svcCloseHandle(httpc_sslc_handle);
		return ret;
	}

	ropvmem_sharedmem = (vu32*)mappableAlloc(ropvmem_size);
	if(ropvmem_sharedmem==NULL)
	{
		ret = -3;
		mappableFree((void*)httpheap_sharedmem);
		svcCloseHandle(httpheap_sharedmem_handle);
		svcCloseHandle(ropvmem_sharedmem_handle);
		svcCloseHandle(httpc_sslc_handle);
		return ret;
	}

	if(R_FAILED(ret=svcMapMemoryBlock(httpheap_sharedmem_handle, (u32)httpheap_sharedmem, MEMPERM_READ | MEMPERM_WRITE, MEMPERM_READ | MEMPERM_WRITE)))
	{
		svcCloseHandle(httpheap_sharedmem_handle);
		mappableFree((void*)httpheap_sharedmem);
		httpheap_sharedmem = NULL;

		svcCloseHandle(ropvmem_sharedmem_handle);
		mappableFree((void*)ropvmem_sharedmem);
		ropvmem_sharedmem = NULL;

		svcCloseHandle(httpc_sslc_handle);

		printf("svcMapMemoryBlock with the httpheap sharedmem failed: 0x%08x.\n", (unsigned int)ret);
		return ret;
	}

	if(R_FAILED(ret=svcMapMemoryBlock(ropvmem_sharedmem_handle, (u32)ropvmem_sharedmem, MEMPERM_READ | MEMPERM_WRITE, MEMPERM_READ | MEMPERM_WRITE)))
	{
		svcUnmapMemoryBlock(httpheap_sharedmem_handle, (u32)httpheap_sharedmem);
		svcCloseHandle(httpheap_sharedmem_handle);
		mappableFree((void*)httpheap_sharedmem);
		httpheap_sharedmem = NULL;

		svcCloseHandle(ropvmem_sharedmem_handle);
		mappableFree((void*)ropvmem_sharedmem);
		ropvmem_sharedmem = NULL;

		svcCloseHandle(httpc_sslc_handle);

		printf("svcMapMemoryBlock with the ropvmem sharedmem failed: 0x%08x.\n", (unsigned int)ret);
		return ret;
	}

	printf("Finishing haxx setup with sysmodule memory...\n");
	ret = setuphaxx_httpheap_sharedmem(first_targeturlctx);

	if(R_FAILED(ret))
	{
		printf("Failed to finish haxx setup: 0x%08x.\n", (unsigned int)ret);
	}
	else
	{
		printf("Finalizing...\n");
	}

	svcUnmapMemoryBlock(httpheap_sharedmem_handle, (u32)httpheap_sharedmem);
	svcCloseHandle(httpheap_sharedmem_handle);
	mappableFree((void*)httpheap_sharedmem);
	httpheap_sharedmem = NULL;

	svcUnmapMemoryBlock(ropvmem_sharedmem_handle, (u32)ropvmem_sharedmem);
	svcCloseHandle(ropvmem_sharedmem_handle);
	mappableFree((void*)ropvmem_sharedmem);
	ropvmem_sharedmem = NULL;

	if(R_FAILED(ret))
	{
		svcCloseHandle(httpc_sslc_handle);
		return ret;
	}

	printf("Running setup with sslc...\n");
	ret = setuphax_http_sslc(httpc_sslc_handle, cert, certsize);

	svcCloseHandle(httpc_sslc_handle);//Normally sslcExit should close this, but close it here too just in case.

	if(R_FAILED(ret))
	{
		printf("Setup failed with sslc: 0x%08x.\n", (unsigned int)ret);
		return ret;
	}

	printf("Testing httpc...\n");

	for(i=0; i<3; i++)
	{
		if(i==2)requrl = "http://localhost/ctr-httpwn/cmdhandler";
		ret = httpcOpenContext(&context, HTTPC_METHOD_POST, requrl, 1);
		if(R_FAILED(ret))
		{
			printf("httpcOpenContext returned 0x%08x, i=%u.\n", (unsigned int)ret, (unsigned int)i);
			return ret;
		}

		ret = httpcAddRequestHeaderField(&context, "User-Agent", "ctr-httpwn/"VERSION);
		if(R_FAILED(ret))
		{
			printf("httpcAddRequestHeaderField returned 0x%08x, i=%u.\n", (unsigned int)ret, (unsigned int)i);
			httpcCloseContext(&context);
			return ret;
		}

		if(i!=2)//Normal httpc commands shouldn't be used with the custom cmdhandler session-handle at this point, since memory will be left mapped in http-sysmodule.
		{
			ret = httpcAddPostDataAscii(&context, "form_name", "form_value");
			if(R_FAILED(ret))
			{
				printf("httpcAddPostDataAscii returned 0x%08x, i=%u.\n", (unsigned int)ret, (unsigned int)i);
				httpcCloseContext(&context);
				return ret;
			}
		}

		if(i==2)
		{
			ret = test_customcmdhandler(&context);
			if(R_FAILED(ret))
			{
				printf("test_customcmdhandler returned 0x%08x.\n", (unsigned int)ret);
				return ret;
			}
		}

		if(i!=2)httpcCloseContext(&context);
	}

	if(R_SUCCEEDED(ret))
	{
		boss_targeturlctx = config_findurltarget_entry(&first_targeturlctx, NULL, "bosshaxx");
		if(boss_targeturlctx)
		{
			ret = test_boss(boss_targeturlctx->url, &context);
			if(R_FAILED(ret))printf("Reboot your system then try again.\n");
		}
		else
		{
			printf("WARNING: bosshaxx wasn't setup, likely due to incompatible system-version. test_boss() will not be run.\n");
		}
	}

	httpcCloseContext(&context);

	return ret;
}

Result download_config(char *url, u8 *cert, u32 certsize, u8 *filebuffer, u32 dlsize, u32 *out_statuscode)
{
	Result ret=0;
	u32 statuscode=0;
	httpcContext context;

	ret = httpcOpenContext(&context, HTTPC_METHOD_GET, url, 1);
	if(R_FAILED(ret))
	{
		printf("httpcOpenContext returned 0x%08x.\n", (unsigned int)ret);
		return ret;
	}

	ret = httpcAddRequestHeaderField(&context, "User-Agent", "ctr-httpwn/"VERSION);
	if(R_FAILED(ret))
	{
		printf("httpcAddRequestHeaderField returned 0x%08x.\n", (unsigned int)ret);
		httpcCloseContext(&context);
		return ret;
	}

	ret = httpcAddTrustedRootCA(&context, cert, certsize);
	if(R_FAILED(ret))
	{
		printf("httpcAddTrustedRootCA returned 0x%08x.\n", (unsigned int)ret);
		httpcCloseContext(&context);
		return ret;
	}

	ret = httpcBeginRequest(&context);
	if(R_FAILED(ret))
	{
		printf("httpcBeginRequest returned 0x%08x.\n", (unsigned int)ret);
		httpcCloseContext(&context);
		return ret;
	}

	ret = httpcGetResponseStatusCodeTimeout(&context, &statuscode, 30000000000);
	if(R_FAILED(ret))
	{
		printf("httpcGetResponseStatusCode returned 0x%08x.\n", (unsigned int)ret);
		httpcCloseContext(&context);
		return ret;
	}

	if(out_statuscode)*out_statuscode = statuscode;

	if(statuscode==200 || statuscode==500)
	{
		ret = httpcDownloadData(&context, filebuffer, dlsize, NULL);
		if(ret!=0 && statuscode==200)
		{
			printf("httpcDownloadData returned 0x%08x.\n", (unsigned int)ret);
			httpcCloseContext(&context);
			return ret;
		}
	}

	ret = httpcCloseContext(&context);
	if(R_FAILED(ret))
	{
		printf("httpcCloseContext returned 0x%08x.\n", (unsigned int)ret);
		return ret;
	}

	if(statuscode!=200)
	{
		printf("Invalid statuscode: %u.\n", (unsigned int)statuscode);
		return -5;
	}

	return 0;
}

int httpwn_dirfilter(const struct dirent *dirent)//Only return dir-entries with a name ending with ".xml".
{
	size_t len;

	len = strlen(dirent->d_name);
	if(len < 4)return 0;

	if(dirent->d_name[0]=='.')return 0;//Ignore "hidden" dir-entries.

	if(strncmp(&dirent->d_name[len-4], ".xml", 4))return 0;

	return 1;
}

//Originally this was supposed to use scandir() but scandir() isn't actually available. From menuhax_manager.
Result httpwn_scandir(const char *dirpath, struct dirent ***namelist, size_t *total_entries)
{
	Result ret=0;

	size_t pos=0;
	DIR *dirp;
	struct dirent *direntry;

	*total_entries = 0;

	dirp = opendir(dirpath);
	if(dirp==NULL)return errno;

	while((direntry = readdir(dirp)))
	{
		if(httpwn_dirfilter(direntry)==0)continue;

		(*total_entries)++;
	}

	closedir(dirp);

	if(*total_entries == 0)return 0;

	dirp = opendir(dirpath);
	if(dirp==NULL)return errno;

	*namelist = malloc(sizeof(struct dirent *) * (*total_entries));
	if(*namelist == NULL)
	{
		closedir(dirp);
		return -2;
	}

	ret = 0;

	while((direntry = readdir(dirp)) && pos < *total_entries)
	{
		if(httpwn_dirfilter(direntry)==0)continue;

		(*namelist)[pos] = malloc(sizeof(struct dirent));
		if((*namelist)[pos] == NULL)
		{
			ret = -2;
			break;
		}

		memcpy((*namelist)[pos], direntry, sizeof(struct dirent));

		pos++;
	}

	closedir(dirp);

	if(ret!=0)
	{
		for(pos=0; pos<(*total_entries); pos++)free((*namelist)[pos]);
		free(*namelist);
		*namelist = NULL;
	}

	return 0;
}

Result httpwn_setup(char *serverconfig_localpath)
{
	Result ret = 0;
	u64 http_sysmodule_titleid = 0x0004013000002902ULL;
	AM_TitleEntry title_entry;

	u8 *cert = (u8*)builtin_rootca_der;
	u32 certsize = builtin_rootca_der_size;

	u8 *filebuffer;
	u32 filebuffer_size = 0x100000;
	u32 statuscode = 0;

	configctx config;
	targeturlctx *first_targeturlctx = NULL;

	struct dirent **namelist = NULL;
	size_t total_entries=0;
	int pos;

	FILE *f;

	char *strptr;
	char *url = NULL;

	char filepath[256];

	char urlbuf[1024];

	memset(&config, 0, sizeof(configctx));
	config.first_targeturlctx = &first_targeturlctx;

	ret = AM_GetTitleInfo(MEDIATYPE_NAND, 1, &http_sysmodule_titleid, &title_entry);
	if(ret!=0)
	{
		printf("Failed to get the HTTP sysmodule title-version: 0x%08x.\n", (unsigned int)ret);
		return ret;
	}

	http_codebin_buf = NULL;
	http_codebin_buf32 = NULL;
	http_codebin_size = 0;

	ret = loadcodebin(http_sysmodule_titleid, MEDIATYPE_NAND, &http_codebin_buf, &http_codebin_size);
	if(R_FAILED(ret))
	{
		printf("Failed to load the HTTP sysmodule codebin: 0x%08x.\n", (unsigned int)ret);
		return ret;
	}

	http_codebin_buf32 = (u32*)http_codebin_buf;

	ret = httpcInit(0x1000);
	if(ret!=0)
	{
		printf("Failed to initialize HTTPC: 0x%08x.\n", (unsigned int)ret);
		if(ret==0xd8e06406)
		{
			printf("The HTTPC service is inaccessible. With the *hax payload this may happen if the process this app is running under doesn't have access to that service. Please try rebooting the system, boot *hax payload, then directly launch the app.\n");
		}

		free(http_codebin_buf);

		return ret;
	}

	filebuffer = malloc(filebuffer_size);
	if(filebuffer==NULL)
	{
		printf("Failed to allocate the config filebuffer.\n");
		ret = -2;
		httpcExit();
		free(http_codebin_buf);
		return ret;
		
	}
	memset(filebuffer, 0, filebuffer_size);

	f = fopen("romfs:/internal_config.xml", "rb");
	if(f)
	{
		printf("Loading+parsing internal_config.xml...\n");

		memset(filebuffer, 0, filebuffer_size);
		fread(filebuffer, 1, filebuffer_size-1, f);
		fclose(f);

		ret = config_parse(&config, (char*)filebuffer);

		if(ret==0)
		{
			if(display_config_message(&config, "Message from the internal_config:"))
			{
				httpcExit();
				free(http_codebin_buf);
				free(filebuffer);
				config_freemem(&config);
				return 0;
			}
		}
	}
	else
	{
		printf("Failed to open the internal_config.");
		httpcExit();
		free(http_codebin_buf);
		free(filebuffer);
		config_freemem(&config);
		return -10;
	}

	url = "https://yls8.mtheall.com/ctr-httpwn/config.php";

	memset(urlbuf, 0, sizeof(urlbuf));
	f = fopen("url_config.txt", "rb");//This is the url-config filepath from here: https://github.com/skiptirengu/ctr-httpwn
	if(f)
	{
		fread(urlbuf, 1, sizeof(urlbuf)-1, f);
		fclose(f);

		strptr = strchr(urlbuf, '\r');
		if(strptr)*strptr = 0;
		strptr = strchr(urlbuf, '\n');
		if(strptr)*strptr = 0;
		url = urlbuf;

		printf("Using URL loaded from url_config.txt: %s\n", url);
	}

	printf("Downloading config...\n");
	ret = download_config(url, cert, certsize, filebuffer, filebuffer_size-1, &statuscode);
	if(ret!=0)
	{
		printf("Config downloading failed: 0x%08x.\n", (unsigned int)ret);

		if(statuscode==500)
		{
			printf("HTTP status-code 500 was returned, server reply:\n%s\n", (char*)filebuffer);
		}

		f = fopen(serverconfig_localpath, "rb");
		if(f)
		{
			printf("Use the cached server_config from SD instead?\nPress the A button to continue, B to abort.\n");
			while(1)
			{
				gspWaitForVBlank();
				hidScanInput();
				if(hidKeysDown() & KEY_A)break;
				if(hidKeysDown() & KEY_B)
				{
					fclose(f);
					httpcExit();
					free(http_codebin_buf);
					return ret;
				}
			}

			memset(filebuffer, 0, filebuffer_size);
			fread(filebuffer, 1, filebuffer_size-1, f);
			fclose(f);
		}
		else
		{
			httpcExit();
			free(http_codebin_buf);
			config_freemem(&config);
			return ret;
		}
	}
	else
	{
		unlink(serverconfig_localpath);
		f = fopen(serverconfig_localpath, "wb");
		if(f)
		{
			fwrite(filebuffer, 1, strlen((char*)filebuffer), f);
			fclose(f);
		}
	}

	ret = config_parse(&config, (char*)filebuffer);

	if(ret==0)
	{
		if(title_entry.version != 13318)
		{
			printf("The installed HTTP sysmodule version(v%u) is not supported.", title_entry.version);
			if(config.incompatsysver_message[0])printf(" %s", config.incompatsysver_message);
			printf("\n");

			httpcExit();
			free(http_codebin_buf);
			free(filebuffer);

			return -1;
		}

		if(display_config_message(&config, "Message from the server:"))
		{
			httpcExit();
			free(http_codebin_buf);
			free(filebuffer);
			config_freemem(&config);
			return 0;
		}
	}

	if(ret==0)
	{
		mkdir("user_config", 0777);
		rename("user_config.xml", "user_config/user_config.xml");

		ret = httpwn_scandir("user_config", &namelist, &total_entries);
		if(ret!=0)
		{
			ret = 0;
		}
		else if(total_entries)
		{
			for(pos=0; pos<total_entries; pos++)
			{
				memset(filepath, 0, sizeof(filepath));
				snprintf(filepath, sizeof(filepath)-1, "%s/%s", "user_config", namelist[pos]->d_name);

				f = fopen(filepath, "rb");
				if(f)
				{
					printf("Loading+parsing the following from SD: %s\n", filepath);

					memset(filebuffer, 0, filebuffer_size);
					fread(filebuffer, 1, filebuffer_size-1, f);
					fclose(f);

					ret = config_parse(&config, (char*)filebuffer);

					if(ret==0)
					{
						if(display_config_message(&config, "Message from the user_config:"))
						{
							httpcExit();
							free(http_codebin_buf);
							free(filebuffer);
							config_freemem(&config);

							for(pos=0; pos<total_entries; pos++)free(namelist[pos]);
							free(namelist);

							return 0;
						}
					}
				}

				if(ret!=0)break;
			}

			for(pos=0; pos<total_entries; pos++)free(namelist[pos]);
			free(namelist);
		}
	}

	if(ret!=0)
	{
		printf("Config parsing failed: 0x%08x.\n", (unsigned int)ret);
		httpcExit();
		free(http_codebin_buf);
		free(filebuffer);
		config_freemem(&config);
		return ret;
	}

	f = fopen("user_nim_rootcertchain_rootca.der", "rb");
	if(f)
	{
		printf("Loading user_nim_rootcertchain_rootca.der since it exists on SD, which will be used instead of the built-in ctr-httpwn cert...\n");

		memset(filebuffer, 0, filebuffer_size);
		certsize = fread(filebuffer, 1, filebuffer_size, f);
		fclose(f);

		cert = filebuffer;
	}

	printf("Preparing the haxx...\n");
	ret = http_haxx("http://localhost/", cert, certsize, first_targeturlctx);//URL doesn't matter much since this won't actually be requested over the network.
	config_freemem(&config);
	httpcExit();
	free(http_codebin_buf);
	free(filebuffer);
	if(ret!=0)
	{
		printf("Haxx setup failed: 0x%08x.\n", (unsigned int)ret);
		return ret;
	}

	return ret;
}

int main(int argc, char **argv)
{
	int abort=0;
	FILE *f = NULL;
	char *serverconfig_localpath = "server_config.xml";

	Result ret = 0;

	// Initialize services
	gfxInitDefault();

	consoleInit(GFX_TOP, NULL);

	printf("ctr-httpwn %s by yellows8.\n", VERSION);

	f = fopen(serverconfig_localpath, "rb");//Only run the below block when this file doesn't exist, which normally only happens when this app wasn't run before with the config file being downloaded successfully.
	if(f)
	{
		fclose(f);
	}
	else
	{
		printf("Please read the documentation before continuing:\nhttps://github.com/yellows8/ctr-httpwn\nPress A to continue, B to abort.\n");

		while(1)
		{
			gspWaitForVBlank();
			hidScanInput();
			if(hidKeysDown() & KEY_A)break;
			if(hidKeysDown() & KEY_B)
			{
				abort = 1;
				break;
			}
		}
	}

	if(!abort)
	{
		ret = romfsInit();
		if(R_FAILED(ret))printf("romfsInit() failed: 0x%08x.\n", (unsigned int)ret);

		if(R_SUCCEEDED(ret))
		{
			ret = amInit();
			if(ret!=0)
			{
				printf("Failed to initialize AM: 0x%08x.\n", (unsigned int)ret);
				if(ret==0xd8e06406)
				{
					printf("The AM service is inaccessible. With the *hax payloads this should never happen. This is normal with plain ninjhax v1.x: this app isn't usable from ninjhax v1.x without any further hax.\n");
				}
			}

			if(R_SUCCEEDED(ret))
			{
				ret = httpwn_setup(serverconfig_localpath);
				amExit();
			}
		}

		romfsExit();

		if(ret==0)printf("Done.\n");
	}

	if(ret==0xC920181A)printf("This error means the HTTP sysmodule crashed.\n");
	if(ret!=0)printf("An error occured. If this is an actual issue not related to user failure, please report this to here if it persists(or comment on an already existing issue if needed), with a screenshot: https://github.com/yellows8/ctr-httpwn/issues\n");

	printf("Press the START button to exit.\n");
	// Main loop
	while (aptMainLoop())
	{
		gspWaitForVBlank();
		hidScanInput();

		u32 kDown = hidKeysDown();
		if (kDown & KEY_START)
			break; // break in order to return to hbmenu
	}

	// Exit services
	gfxExit();
	return 0;
}

