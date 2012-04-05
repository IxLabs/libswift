/*
 *  storage.cpp
 *  swift
 *
 *  Created by Arno Bakker.
 *  Copyright 2009-2012 TECHNISCHE UNIVERSITEIT DELFT. All rights reserved.
 *
 * TODO:
 * - Unicode?
 * - If multi-file spec, then exact size known after 1st few chunks. Use for swift::Size()?
 */

#include "swift.h"
#include "compat.h"

#include <vector>
#include <utility>

using namespace swift;


const std::string Storage::MULTIFILE_PATHNAME = "META-INF-multifilespec.txt";
const std::string Storage::MULTIFILE_PATHNAME_FILE_SEP = "/";

Storage::Storage(std::string pathname) : state_(STOR_STATE_INIT), spec_size_(0), single_fd_(-1), reserved_size_(-1)
{
	os_pathname_ = pathname;

	int64_t fsize = file_size_by_path(pathname.c_str());
	if (fsize < 0 && errno == ENOENT)
	{
		// File does not exist, assume we're a client and all will be revealed
		// (single file, multi-spec) when chunks come in.
		return;
	}

	// File exists. Check first bytes to see if a multifile-spec
	FILE *fp = fopen(pathname.c_str(),"rb");
	char readbuf[1024];
	int ret = fread(readbuf,sizeof(char),MULTIFILE_PATHNAME.length(),fp);
	fclose(fp);
	if (ret < 0)
		return;

	if (!strncmp(readbuf,MULTIFILE_PATHNAME.c_str(),MULTIFILE_PATHNAME.length()))
	{
		// Pathname points to a multi-file spec, assume we're seeding
		state_ = STOR_STATE_MFSPEC_COMPLETE;

		dprintf("%s %s storage: Found multifile-spec, will seed it.\n", tintstr(), roothashhex().c_str() );

		StorageFile *sf = new StorageFile(pathname,0,fsize);
		sfs_.push_back(sf);
		if (ParseSpec(sf) < 0)
			print_error("storage: error parsing multi-file spec");
	}
	else
	{
		// Normal swarm
		dprintf("%s %s storage: Found single file, will check it.\n", tintstr(), roothashhex().c_str() );

		(void)OpenSingleFile(); // sets state to STOR_STATE_SINGLE_FILE
	}
}


Storage::~Storage()
{
	if (single_fd_ != -1)
		close(single_fd_);

	storage_files_t::iterator iter;
	for (iter = sfs_.begin(); iter < sfs_.end(); iter++)
	{
		StorageFile *sf = *iter;
		delete sf;
	}
	sfs_.clear();
}


ssize_t  Storage::Write(const void *buf, size_t nbyte, int64_t offset)
{
	//dprintf("%s %s storage: Write: nbyte %d off %lld\n", tintstr(), roothashhex().c_str(), nbyte,offset);

	if (state_ == STOR_STATE_SINGLE_FILE)
	{
		return pwrite(single_fd_, buf, nbyte, offset);
	}
	// MULTIFILE
	if (state_ == STOR_STATE_INIT)
	{
		if (offset != 0)
		{
			errno = EINVAL;
			return -1;
		}

		//dprintf("%s %s storage: Write: chunk 0\n");

		// Check for multifile spec. If present, multifile, otherwise single
		if (!strncmp((const char *)buf,MULTIFILE_PATHNAME.c_str(),strlen(MULTIFILE_PATHNAME.c_str())))
		{
			dprintf("%s %s storage: Write: Is multifile\n", tintstr(), roothashhex().c_str() );

			// multifile entry will fit into first chunk
			const char *bufstr = (const char *)buf;
			int n = sscanf((const char *)&bufstr[strlen(MULTIFILE_PATHNAME.c_str())+1],"%lld",&spec_size_);
			if (n != 1)
			{
				errno = EINVAL;
				return -1;
			}

			//dprintf("%s %s storage: Write: multifile: specsize %lld\n", tintstr(), roothashhex().c_str(), spec_size_ );

			// Create StorageFile for multi-file spec.
			StorageFile *sf = new StorageFile(MULTIFILE_PATHNAME.c_str(),0,spec_size_);
			sfs_.push_back(sf);

			// Write all, or part of spec and set state_
			return WriteSpecPart(sf,buf,nbyte,offset);
		}
		else
		{
			// Is a single file swarm.
			int ret = OpenSingleFile(); // sets state to STOR_STATE_SINGLE_FILE
			if (ret < 0)
				return -1;

			// Write chunk to file via recursion.
			return Write(buf,nbyte,offset);
		}
	}
	else if (state_ == STOR_STATE_MFSPEC_SIZE_KNOWN)
	{
		StorageFile *sf = sfs_[0];

		dprintf("%s %s storage: Write: mf spec size known\n", tintstr(), roothashhex().c_str());

		return WriteSpecPart(sf,buf,nbyte,offset);
	}
	else
	{
		// state_ == STOR_STATE_MFSPEC_COMPLETE;
		//dprintf("%s %s storage: Write: complete\n", tintstr(), roothashhex().c_str());

		StorageFile *sf = FindStorageFile(offset);
		if (sf == NULL)
		{
			dprintf("%s %s storage: Write: File not found!\n", tintstr(), roothashhex().c_str());
			errno = EINVAL;
			return -1;
		}

		std::pair<int64_t,int64_t> ht = WriteBuffer(sf,buf,nbyte,offset);
		if (ht.first == -1)
		{
			errno = EINVAL;
			return -1;
		}

		//dprintf("%s %s storage: Write: complete: first %lld second %lld\n", tintstr(), roothashhex().c_str(), ht.first, ht.second);

		if (ht.second > 0)
		{
			// Write tail to next StorageFile(s) using recursion
			const char *bufstr = (const char *)buf;
			int ret = Write(&bufstr[ht.first], ht.second, offset+ht.first );
			if (ret < 0)
				return ret;
			else
				return ht.first+ret;
		}
		else
			return ht.first;
	}
}


int Storage::WriteSpecPart(StorageFile *sf, const void *buf, size_t nbyte, int64_t offset)
{
	dprintf("%s %s storage: WriteSpecPart: %s %d %lld\n", tintstr(), roothashhex().c_str(), sf->GetSpecPathName().c_str(), nbyte, offset );

	std::pair<int64_t,int64_t> ht = WriteBuffer(sf,buf,nbyte,offset);
	if (ht.first == -1)
	{
		errno = EINVAL;
		return -1;
	}

	if (offset+ht.first == sf->GetEnd()+1)
	{
		// Wrote last part of spec
		state_ = STOR_STATE_MFSPEC_COMPLETE;

		if (ParseSpec(sf) < 0)
		{
			errno = EINVAL;
			return -1;
		}

		// Write tail to next StorageFile(s) using recursion
		const char *bufstr = (const char *)buf;
		int ret = Write(&bufstr[ht.first], ht.second, offset+ht.first );
		if (ret < 0)
			return ret;
		else
			return ht.first+ret;
	}
	else
	{
		state_ = STOR_STATE_MFSPEC_SIZE_KNOWN;
		return ht.first;
	}
}



std::pair<int64_t,int64_t> Storage::WriteBuffer(StorageFile *sf, const void *buf, size_t nbyte, int64_t offset)
{
	//dprintf("%s %s storage: WriteBuffer: %s %d %lld\n", tintstr(), roothashhex().c_str(), sf->GetSpecPathName().c_str(), nbyte, offset );

	int ret = -1;
	if (offset+nbyte <= sf->GetEnd()+1)
	{
		// Chunk belongs completely in sf
		ret = sf->Write(buf,nbyte,offset - sf->GetStart());

		//dprintf("%s %s storage: WriteBuffer: Write: covered ret %d\n", tintstr(), roothashhex().c_str(), ret );

		if (ret < 0)
			return std::make_pair(-1,-1);
		else
			return std::make_pair(nbyte,0);

	}
	else
	{
		int64_t head = sf->GetEnd()+1 - offset;
		int64_t tail = nbyte - head;

		// Write last part of file
		ret = sf->Write(buf,head,offset - sf->GetStart() );

		//dprintf("%s %s storage: WriteBuffer: Write: partial ret %d\n", tintstr(), roothashhex().c_str(), ret );

		if (ret < 0)
			return std::make_pair(-1,-1);
		else
			return std::make_pair(head,tail);
	}
}




StorageFile * Storage::FindStorageFile(int64_t offset)
{
	// Binary search for StorageFile that manages the given offset
	int imin = 0, imax=sfs_.size()-1;
	while (imax >= imin)
	{
		int imid = (imin + imax) / 2;
		if (offset >= sfs_[imid]->GetEnd()+1)
			imin = imid + 1;
		else if (offset < sfs_[imid]->GetStart())
			imax = imid - 1;
		else
			return sfs_[imid];
	}
	// Should find it.
	return NULL;
}


int Storage::ParseSpec(StorageFile *sf)
{
	char *retstr = NULL,line[MULTIFILE_MAX_LINE+1];
	FILE *fp = fopen(sf->GetSpecPathName().c_str(),"rb"); // FAXME decode UTF-8
	if (fp == NULL)
	{
		print_error("cannot open multifile-spec");
		return -1;
	}

	int64_t offset=0;
	int ret=0;
	while(1)
	{
		retstr = fgets(line,MULTIFILE_MAX_LINE,fp);
		if (retstr == NULL)
			break;

		// Format: "specpath filesize\n"
		std::string pline(line);
		size_t idx = pline.rfind(' ',pline.length()-1);

		std::string specpath = pline.substr(0,idx);
		std::string sizestr = pline.substr(idx+1,pline.length());

		int64_t fsize=0;
        int n = sscanf(sizestr.c_str(),"%lld",&fsize);
        if (n == 0)
        {
        	ret = -1;
        	break;
        }

        // Check pathname safety
        if (specpath.substr(0,1) == MULTIFILE_PATHNAME_FILE_SEP)
        {
        	// Must not start with /
        	ret = -1;
        	break;
        }
    	idx = specpath.find("..",0);
    	if (idx != std::string::npos)
        {
    		// Must not contain .. path escapes
        	ret = -1;
        	break;
        }

		if (offset == 0)
		{
			// sf already created for multifile-spec entry
			offset += sf->GetSize();
		}
		else
		{
			StorageFile *sf = new StorageFile(specpath,offset,fsize);
			sfs_.push_back(sf);
			offset += fsize;
		}
	}

	// Assume: Multi-file spec sorted, so vector already sorted on offset
	storage_files_t::iterator iter;
	for (iter = sfs_.begin(); iter < sfs_.end(); iter++)
	{
		StorageFile *sf = *iter;
		dprintf("%s %s storage: parsespec: Got %s start %lld size %lld\n", tintstr(), roothashhex().c_str(), sf->GetSpecPathName().c_str(), sf->GetStart(), sf->GetSize() );
	}


	fclose(fp);
	return ret;
}


int Storage::OpenSingleFile()
{
	state_ = STOR_STATE_SINGLE_FILE;

	single_fd_ = open(os_pathname_.c_str(),OPENFLAGS,S_IRUSR|S_IWUSR|S_IRGRP|S_IROTH);
	if (single_fd_<0) {
		single_fd_ = -1;
		print_error("storage: cannot open single file");
	}

	// Perform postponed resize.
	if (reserved_size_ != -1)
	{
		int ret = ResizeReserved(reserved_size_);
		if (ret < 0)
			return ret;
	}

	return single_fd_;
}




ssize_t  Storage::Read(void *buf, size_t nbyte, int64_t offset)
{
	//dprintf("%s %s storage: Read: nbyte " PRISIZET " off %lld\n", tintstr(), roothashhex().c_str(), nbyte, offset );

	if (state_ == STOR_STATE_SINGLE_FILE)
	{
		return pread(single_fd_, buf, nbyte, offset);
	}

	// MULTIFILE
	if (state_ == STOR_STATE_INIT)
	{
		errno = EINVAL;
		return -1;
	}
	else
	{
		StorageFile *sf = FindStorageFile(offset);
		if (sf == NULL)
		{
			errno = EINVAL;
			return -1;
		}

		//dprintf("%s %s storage: Read: Found file %s for off %lld\n", tintstr(), roothashhex().c_str(), sf->GetSpecPathName().c_str(), offset );

		ssize_t ret = sf->Read(buf,nbyte,offset - sf->GetStart());
		if (ret < 0)
			return ret;

		//dprintf("%s %s storage: Read: read %d\n", tintstr(), roothashhex().c_str(), ret );

		if (ret < nbyte && offset+ret != ht_->size())
		{
			//dprintf("%s %s storage: Read: want %d more\n", tintstr(), roothashhex().c_str(), nbyte-ret );

			// Not at end, and can fit more in buffer. Do recursion
			char *bufstr = (char *)buf;
			ssize_t newret = Read((void *)(bufstr+ret),nbyte-ret,offset+ret);
			if (newret < 0)
				return newret;
			else
				return ret + newret;
		}
		else
			return ret;
	}
}


int64_t Storage::GetReservedSize()
{
	if (state_ == STOR_STATE_SINGLE_FILE)
	{
		return file_size(single_fd_);
	}
	else if (state_ != STOR_STATE_MFSPEC_COMPLETE)
		return -1;

	// MULTIFILE
	storage_files_t::iterator iter;
	int64_t totaldisksize=0;
	for (iter = sfs_.begin(); iter < sfs_.end(); iter++)
	{
		StorageFile *sf = *iter;
		int64_t fsize = file_size_by_path( sf->GetSpecPathName().c_str() );
		if( fsize < 0)
		{
			dprintf("%s %s storage: getsize: cannot stat file %s\n", tintstr(), roothashhex().c_str(), sf->GetSpecPathName().c_str() );
			return fsize;
		}
		else
			totaldisksize += fsize;
	}

	return totaldisksize;
}


int Storage::ResizeReserved(int64_t size)
{
	if (state_ == STOR_STATE_SINGLE_FILE)
	{
		dprintf("%s %s storage: Resizing single file %d to %lld\n", tintstr(), roothashhex().c_str(), single_fd_, size);
		return file_resize(single_fd_,size);
	}
	else if (state_ == STOR_STATE_INIT)
	{
		dprintf("%s %s storage: Postpone resize to %lld\n", tintstr(), roothashhex().c_str(), size);
		reserved_size_ = size;
		return 0;
	}
	else if (state_ != STOR_STATE_MFSPEC_COMPLETE)
		return -1;

	// MULTIFILE
	if (size > GetReservedSize())
	{
		dprintf("%s %s storage: Resizing multi file to %lld\n", tintstr(), roothashhex().c_str(), size);

		// Resize files to wanted size, so pread() / pwrite() works for all offsets.
		storage_files_t::iterator iter;
		for (iter = sfs_.begin(); iter < sfs_.end(); iter++)
		{
			StorageFile *sf = *iter;
			int ret = sf->ResizeReserved();
			if (ret < 0)
				return ret;
		}
	}
	else
		dprintf("%s %s storage: Resize multi-file to smaller %lld, ignored\n", tintstr(), roothashhex().c_str(), size);

	return 0;
}


std::string Storage::spec2ospn(std::string specpn)
{
	std::string dest = specpn;
	// TODO: convert to UTF-8
	if (MULTIFILE_PATHNAME_FILE_SEP != FILE_SEP)
	{
		// Replace OS filesep with spec
		swift::stringreplace(dest,MULTIFILE_PATHNAME_FILE_SEP,FILE_SEP);
	}
	return dest;
}

std::string Storage::os2specpn(std::string ospn)
{
	std::string dest = ospn;
	// TODO: convert to UTF-8
	if (MULTIFILE_PATHNAME_FILE_SEP != FILE_SEP)
	{
		// Replace OS filesep with spec
		swift::stringreplace(dest,FILE_SEP,MULTIFILE_PATHNAME_FILE_SEP);
	}
	return dest;
}



/*
 * StorageFile
 */



StorageFile::StorageFile(std::string utf8path, int64_t start, int64_t size) : fd_(-1)
{
	spec_pathname_ = utf8path;
	start_ = start;
	end_ = start+size-1;

	// Convert specname to OS name
	std::string ospathname = Storage::spec2ospn(spec_pathname_);

	// Handle subdirs
	if (ospathname.find(FILE_SEP,0) != std::string::npos)
	{
		// Path contains dirs, make them
		size_t i = 0;
		while(true)
		{
			i = ospathname.find(FILE_SEP,i+1);
			if (i == std::string::npos)
				 break;
			std::string path = ospathname.substr(0,i);
#ifdef WIN32
			struct _stat statbuf;
#else
			struct stat statbuf;
#endif
			int ret = stat( path.c_str(), &statbuf );
			if (ret < 0 && errno == ENOENT)
			{
#ifdef WIN32
				ret = _mkdir(path.c_str());
#else
				ret = mkdir(path.c_str(),S_IRUSR|S_IWUSR|S_IRGRP|S_IROTH);
#endif
				if (ret < 0)
					return;
			}
			else if (ret == 0 && !(statbuf.st_mode & S_IFDIR))
			{
				// Something already exists and it is not a dir
				return;
			}
		}
	}


	// Open
	fd_ = open(ospathname.c_str(),OPENFLAGS,S_IRUSR|S_IWUSR|S_IRGRP|S_IROTH);
	if (fd_<0) {
		//print_error("storage: file: Could not open");
		dprintf("%s %s storage: file: Could not open %s\n", tintstr(), "0000000000000000000000000000000000000000", ospathname.c_str() );
        return;
	}
}

StorageFile::~StorageFile()
{
	 if (fd_ < 0)
		 close(fd_);
}


