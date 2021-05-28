/***************************************************************************\
    imgdb.cpp - iqdb library implementation

    Copyright (C) 2008 piespy@gmail.com

    Originally based on imgSeek code, these portions
    Copyright (C) 2003 Ricardo Niederberger Cabral.

    This program is free software; you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation; either version 2 of the License, or
    (at your option) any later version.

    This program is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with this program; if not, write to the Free Software
    Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
\**************************************************************************/

/* System includes */
#include <ctime>
#include <errno.h>
#include <fcntl.h>
#include <limits>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <time.h>

/* STL includes */
#include <algorithm>
#include <cmath>
#include <vector>

/* iqdb includes */
#include "debug.h"
#include "imgdb.h"
#include "imglib.h"

extern int debug_level;

namespace imgdb {

//#define DEBUG_QUERY

// Globals
//keywordsMapType globalKwdsMap;
#ifdef INTMATH
Score weights[2][6][3];

#define ScD(x) ((double)(x) / imgdb::ScoreMax)
#define DScD(x) ScD(((x) / imgdb::ScoreMax))
#define DScSc(x) ((x) >> imgdb::ScoreScale)
#else
#define weights weightsf

#define ScD(x) (x)
#define DScD(x) (x)
#define DScSc(x) (x)
#endif

/* Fixed weight mask for pixel positions (i,j).
Each entry x = i*NUM_PIXELS + j, gets value max(i,j) saturated at 5.
To be treated as a constant.
 */
unsigned char imgBin[NUM_PIXELS * NUM_PIXELS];
int imgBinInited = 0;

const static bool is_disk_db = true;
static size_t pageSize = 0;
static size_t pageMask = 0;
static size_t pageImgs = 0;
static size_t pageImgMask = 0;

mapped_file::mapped_file(const char *fname, bool writable) {
  int fd = open(fname, writable ? O_RDWR : O_RDONLY);
  struct stat st;
  if (fd == -1 || fstat(fd, &st) || (m_base = (unsigned char *)mmap(NULL, m_length = st.st_size, writable ? PROT_READ | PROT_WRITE : PROT_READ, MAP_SHARED, fd, 0)) == MAP_FAILED)
    throw image_error("Can't open/stat/map file.");
  close(fd);
}

inline void mapped_file::unmap() {
  if (!m_base)
    return;
  if (munmap(m_base, m_length))
    DEBUG(errors)("WARNING: Could not unmap %zd bytes of memory.\n", m_length);
}

int tempfile() {
  char tempnam[] = "/tmp/imgdb_cache.XXXXXXX";
  int fd = mkstemp(tempnam);
  if (fd == -1)
    throw io_error(std::string("Can't open cache file ") + tempnam + ": " + strerror(errno));
  if (unlink(tempnam))
    DEBUG(errors)("WARNING: Can't unlink cache file %s: %s.", tempnam, strerror(errno));
  return fd;
}

template <bool is_simple>
int imageIdIndex_list<is_simple, false>::m_fd = -1;

template <bool is_simple>
void imageIdIndex_list<is_simple, false>::resize(size_t s) {
  if (!is_simple)
    s = (s + pageImgMask) & ~pageImgMask;
  if (s <= m_capacity)
    return;
  if (m_fd == -1)
    m_fd = tempfile();
  size_t toadd = s - m_capacity;
  off_t page = lseek(m_fd, 0, SEEK_CUR);
  //fprintf(stderr, "%zd/%zd entries at %llx=", toadd, s, page);
  m_baseofs = page & pageMask;
  size_t len = toadd * sizeof(imageId);
  m_capacity += len / sizeof(imageId);
  if (m_baseofs & (sizeof(imageId) - 1))
    throw internal_error("Mis-aligned file position.");
  if (!is_simple && m_baseofs)
    throw internal_error("Base offset in write mode.");
  page &= ~pageMask;
  //fprintf(stderr, "%llx:%zx, %zd bytes=%zd.\n", page, m_baseofs, len, m_capacity);
  if (ftruncate(m_fd, lseek(m_fd, len, SEEK_CUR)))
    throw io_error("Failed to resize bucket map file.");
  m_pages.push_back(imageIdPage(page, len));
}

template <>
imageIdIndex_map<true> imageIdIndex_list<true, false>::map_all(bool writable) {
  //fprintf(stderr, "Mapping... write=%d size=%zd+%zd=%zd/%zd. %zd pages. ", writable, m_size, m_tail.size(), size(), m_capacity, m_pages.size());
  if (!writable && !size())
    return imageIdIndex_map<true>();
  if (!writable && m_pages.empty()) {
    //fprintf(stderr, "Using fake map of tail data.\n");
    return imageIdIndex_map<true>();
  }
  imageIdPage &page = m_pages.front();
  size_t length = page.second + m_baseofs;
  //fprintf(stderr, "Directly mapping %zd bytes. ", length);
  void *base = mmap(NULL, length, PROT_READ | PROT_WRITE, MAP_SHARED, m_fd, page.first);
  if (base == MAP_FAILED)
    throw memory_error("Failed to mmap bucket.");
  return imageIdIndex_map<true>(base, (size_t *)(((char *)base) + m_baseofs), (size_t *)(((char *)base) + m_baseofs) + m_size, length);
}

template <>
imageIdIndex_map<false> imageIdIndex_list<false, false>::map_all(bool writable) {
  if (!writable && !size())
    return imageIdIndex_map<false>();
  if (!writable && m_pages.empty()) {
    //fprintf(stderr, "Using fake map of tail data.\n");
    return imageIdIndex_map<false>();
  }

  while (writable && !m_tail.empty())
    page_out();

  if (m_baseofs)
    throw internal_error("Base offset in write mode.");
  size_t len = m_capacity * sizeof(imageId);
  len = (len + pageMask) & ~pageMask;
  //fprintf(stderr, "Making full map of %zd bytes. ", len);
  void *base = mmap(NULL, len, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
  if (base == MAP_FAILED)
    throw memory_error("Failed to mmap bucket.");

  imageIdIndex_map<false> mapret(base, (image_id_index *)base, (image_id_index *)base + m_capacity, len);
  char *chunk = (char *)base;
  for (page_list::iterator itr = m_pages.begin(); itr != m_pages.end(); ++itr) {
    //fprintf(stderr, "Using %zd bytes from ofs %llx. ", itr->second, (long long int) itr->first);
    void *map = mmap(chunk, itr->second, PROT_READ | PROT_WRITE, MAP_FIXED | MAP_SHARED, m_fd, itr->first);
    if (map == MAP_FAILED) {
      mapret.unmap();
      throw memory_error("Failed to mmap bucket chunk.");
    }
    chunk += itr->second;
  }
  return mapret;
}

template <bool is_simple>
void imageIdIndex_list<is_simple, false>::page_out() {
  //fprintf(stderr, "Tail has %zd/%zd values. Capacity %zd. Paging out. ", m_tail.size(), size(), m_capacity);
  size_t last = m_size & pageImgMask;
  //fprintf(stderr, "Last page has %zd/%zd(%zd), ", last, m_size, m_capacity);
  if (!last)
    resize(size());
  imageIdPage page = m_pages.back();
  if (((size() + pageImgMask) & ~pageImgMask) != m_capacity) {
    //fprintf(stderr, "Need middle page for %zd(%zd)/%zd.", m_size, (size() + pageImgMask) & ~pageImgMask, m_capacity);
    size_t ofs = last;
    for (page_list::iterator itr = m_pages.begin(); itr != m_pages.end(); ++itr) {
      page = *itr;
      //fprintf(stderr, " %zd@%llx", page.second / sizeof(imageId), (long long int) page.first);
      while (page.second && m_size > ofs) {
        ofs += pageImgs;
        page.first += pageSize;
        page.second -= pageSize;
      }
      if (ofs >= m_size)
        break;
    }
    //fprintf(stderr, " -> %llx = %zd. ", (long long int) page.first, ofs);
    if (ofs != m_size)
      throw internal_error("Counted pages badly.");
  } else {
    //fprintf(stderr, "map @%llx/%zd. ", (long long int) page.first + page.second - pageSize, page.second);
    page.first += page.second - pageSize;
  }

  image_id_index *ptr = (image_id_index *)mmap(NULL, pageSize, PROT_READ | PROT_WRITE, MAP_SHARED, m_fd, page.first);
  if (ptr == MAP_FAILED)
    throw memory_error("Failed to map tail page.");
  size_t copy = std::min(m_tail.size(), pageImgs - last);
  //fprintf(stderr, "Fits %zd, ", copy);
  memcpy(ptr + last, &m_tail.front(), copy * sizeof(imageId));
  m_size += copy;
  if (copy == m_tail.size()) {
    m_tail.clear();
    //fprintf(stderr, "has all. Now %zd.\n", size());
  } else {
    m_tail.erase(m_tail.begin(), m_tail.begin() + copy);
    //fprintf(stderr, "Only fits %zd, %zd left = %zd.\n", copy, m_tail.size(), size());
  }
  if (munmap(ptr, pageSize))
    DEBUG(errors)("WARNING: Failed to munmap tail page.\n");
}

template <>
void imageIdIndex_list<false, false>::remove(image_id_index i) {
  if (!size())
    return;
  //fprintf(stderr, "Removing %lx from %zd/%zd(%zd). ", i.id, m_tail.size(), m_size, m_capacity);
  IdIndex_list::iterator itr = std::find_if(m_tail.begin(), m_tail.end(), std::bind2nd(std::equal_to<image_id_index>(), i));
  if (itr != m_tail.end()) {
    //fprintf(stderr, "Found in tail at %d/%zd.\n", itr - m_tail.begin(), m_tail.size());
    *itr = m_tail.back();
    m_tail.pop_back();
    return;
  }
  AutoImageIdIndex_map<false> map(map_all(false));
  imageIdIndex_map<false>::iterator mItr = map.begin();
  while (mItr != map.end() && *mItr != i)
    ++mItr;
  if (mItr != map.end()) {
    if (*mItr != i)
      throw internal_error("Huh???");
    //fprintf(stderr, "Found at %zd/%zd.\n", ofs, m_size);
    if (!m_tail.empty()) {
      *mItr = m_tail.back();
      m_tail.pop_back();
    } else {
      imageIdIndex_map<false>::iterator last = map.end();
      --last;
      *mItr = *last;
      m_size--;
    }
  }
  //else fprintf(stderr, "NOT FOUND!!!\n");
}

void imageIdIndex_list<true, true>::set_base() {
  if (!m_base.empty())
    return;

  if (m_tail.base_size() * 17 / 16 + 16 < m_tail.base_capacity()) {
    container copy;
    copy.reserve(m_tail.base_size(), true);
    for (container::iterator itr = m_tail.begin(); itr != m_tail.end(); ++itr)
      copy.push_back(*itr);

    m_base.swap(copy);
    copy = container();
    m_tail.swap(copy);
  } else {
    m_base.swap(m_tail);
  }
}

int dbSpace::mode_from_name(const char *mode_name) {
  if (!strcmp(mode_name, "normal"))
    return imgdb::dbSpace::mode_normal;
  else if (!strcmp(mode_name, "readonly"))
    return imgdb::dbSpace::mode_readonly;
  else if (!strcmp(mode_name, "simple"))
    return imgdb::dbSpace::mode_simple;
  else if (!strcmp(mode_name, "alter"))
    return imgdb::dbSpace::mode_alter;
  else if (!strcmp(mode_name, "imgdata"))
    return imgdb::dbSpace::mode_imgdata;
  else
    throw param_error("Unknown mode name.");
}

// Specializations accessing images as SigStruct* or size_t map, and imageIdIndex_map as imageId or index map.
template <>
inline dbSpaceImpl<false>::imageIterator dbSpaceImpl<false>::image_begin() { return imageIterator(m_images.begin(), *this); }
template <>
inline dbSpaceImpl<false>::imageIterator dbSpaceImpl<false>::image_end() { return imageIterator(m_images.end(), *this); }
template <>
inline dbSpaceImpl<true>::imageIterator dbSpaceImpl<true>::image_begin() { return imageIterator(m_info.begin(), *this); }
template <>
inline dbSpaceImpl<true>::imageIterator dbSpaceImpl<true>::image_end() { return imageIterator(m_info.end(), *this); }

template <bool is_simple>
inline typename dbSpaceImpl<is_simple>::imageIterator dbSpaceImpl<is_simple>::find(imageId i) {
  map_iterator itr = m_images.find(i);
  if (itr == m_images.end())
    throw invalid_id("Invalid image ID.");
  return imageIterator(itr, *this);
}

inline dbSpaceAlter::ImageMap::iterator dbSpaceAlter::find(imageId i) {
  ImageMap::iterator itr = m_images.find(i);
  if (itr == m_images.end())
    throw invalid_id("Invalid image ID.");
  return itr;
}

void initImgBin() {
  imgBinInited = 1;
  srand((unsigned)time(0));

  pageSize = sysconf(_SC_PAGESIZE);
  pageMask = pageSize - 1;
  pageImgs = pageSize / sizeof(imageId);
  pageImgMask = pageMask / sizeof(imageId);
  //fprintf(stderr, "page size = %zx, page mask = %zx.\n", pageSize, pageMask);

  /* setup initial fixed weights that each coefficient represents */
  int i, j;

  /*
	0 1 2 3 4 5 6 i
	0 0 1 2 3 4 5 5 
	1 1 1 2 3 4 5 5
	2 2 2 2 3 4 5 5
	3 3 3 3 3 4 5 5
	4 4 4 4 4 4 5 5
	5 5 5 5 5 5 5 5
	5 5 5 5 5 5 5 5
	j
	 */

  /* Every position has value 5, */
  memset(imgBin, 5, NUM_PIXELS_SQUARED);

  /* Except for the 5 by 5 upper-left quadrant: */
  for (i = 0; i < 5; i++)
    for (j = 0; j < 5; j++)
      imgBin[i * NUM_PIXELS + j] = std::max(i, j);
      // Note: imgBin[0] == 0

      /* Integer weights. */
#ifdef INTMATH
  for (i = 0; i < 2; i++)
    for (j = 0; j < 6; j++)
      for (int c = 0; c < 3; c++)
        weights[i][j][c] = lrint(weightsf[i][j][c] * ScoreMax);
#endif
}

template <bool is_simple>
bool dbSpaceImpl<is_simple>::hasImage(imageId id) {
  return m_images.find(id) != m_images.end();
}

bool dbSpaceAlter::hasImage(imageId id) {
  return m_images.find(id) != m_images.end();
}

template <bool is_simple>
inline ImgData dbSpaceImpl<is_simple>::get_sig_from_cache(imageId id) {
  if (is_simple && m_sigFile == -1)
    throw usage_error("Not supported in simple mode.");
  ImgData sig;
  read_sig_cache(find(id).cOfs(), &sig);
  return sig;
}

inline ImgData dbSpaceAlter::get_sig(size_t ind) {
  ImgData sig;
  m_f->seekg(m_sigOff + ind * sizeof(ImgData));
  m_f->read(&sig);
  return sig;
}

template <bool is_simple>
int dbSpaceImpl<is_simple>::getImageWidth(imageId id) {
  return find(id).width();
}

template <bool is_simple>
int dbSpaceImpl<is_simple>::getImageHeight(imageId id) {
  return find(id).height();
}

int dbSpaceAlter::getImageWidth(imageId id) {
  return get_sig(find(id)->second).width;
}

int dbSpaceAlter::getImageHeight(imageId id) {
  return get_sig(find(id)->second).height;
}

inline bool dbSpaceCommon::is_grayscale(const lumin_native &avgl) {
  return std::abs(avgl.v[1]) + std::abs(avgl.v[2]) < MakeScore(6) / 1000;
}

bool dbSpaceCommon::isImageGrayscale(imageId id) {
  lumin_native avgl;
  getImgAvgl(id, avgl);
  return is_grayscale(avgl);
}

void dbSpaceCommon::sigFromImage(Image *image, imageId id, ImgData *sig) {
  AutoCleanArray<unsigned char> rchan(NUM_PIXELS * NUM_PIXELS);
  AutoCleanArray<unsigned char> gchan(NUM_PIXELS * NUM_PIXELS);
  AutoCleanArray<unsigned char> bchan(NUM_PIXELS * NUM_PIXELS);

  sig->id = id;
  sig->width = image->sx;
  sig->height = image->sy;

  AutoGDImage resized;
  if (image->sx != NUM_PIXELS || image->sy != NUM_PIXELS || !gdImageTrueColor(image)) {
    resized.set(gdImageCreateTrueColor(NUM_PIXELS, NUM_PIXELS));
    gdImageFilledRectangle(resized, 0, 0, NUM_PIXELS, NUM_PIXELS, gdTrueColor(255, 255, 255));
    gdImageCopyResampled(resized, image, 0, 0, 0, 0, NUM_PIXELS, NUM_PIXELS, image->sx, image->sy);
    image = resized;
  }

  unsigned char *red = rchan.ptr(), *green = gchan.ptr(), *blue = bchan.ptr();
  for (int **row = image->tpixels; row < image->tpixels + NUM_PIXELS; row++)
    for (int *col = *row, *end = *row + NUM_PIXELS; col < end; col++) {
      *red++ = gdTrueColorGetRed(*col);
      *green++ = gdTrueColorGetGreen(*col);
      *blue++ = gdTrueColorGetBlue(*col);
    }

  AutoCleanArray<Unit> cdata1(NUM_PIXELS * NUM_PIXELS);
  AutoCleanArray<Unit> cdata2(NUM_PIXELS * NUM_PIXELS);
  AutoCleanArray<Unit> cdata3(NUM_PIXELS * NUM_PIXELS);
  transformChar(rchan.ptr(), gchan.ptr(), bchan.ptr(), cdata1.ptr(), cdata2.ptr(), cdata3.ptr());
  calcHaar(cdata1.ptr(), cdata2.ptr(), cdata3.ptr(), sig->sig1, sig->sig2, sig->sig3, sig->avglf);
}

template <typename B>
inline void dbSpaceCommon::bucket_set<B>::add(const ImgData &nsig, count_t index) {
  lumin_native avgl;
  SigStruct::avglf2i(nsig.avglf, avgl);
  for (int i = 0; i < NUM_COEFS; i++) { // populate buckets

#ifdef FAST_POW_GEERT
    int x, t;
    // sig[i] never 0
    int x, t;

    x = nsig.sig1[i];
    t = (x < 0); /* t = 1 if x neg else 0 */
    /* x - 0 ^ 0 = x; i - 1 ^ 0b111..1111 = 2-compl(x) = -x */
    x = (x - t) ^ -t;
    buckets[0][t][x].add(nsig.id, index);

    if (is_grayscale(avg))
      continue; // ignore I/Q coeff's if chrominance too low

    x = nsig.sig2[i];
    t = (x < 0);
    x = (x - t) ^ -t;
    buckets[1][t][x].add(nsig.id, index);

    x = nsig.sig3[i];
    t = (x < 0);
    x = (x - t) ^ -t;
    buckets[2][t][x].add(nsig.id, index);

    should not fail

#else //FAST_POW_GEERT \
    //imageId_array3 (imgbuckets = dbSpace[dbId]->(imgbuckets;
    if (nsig.sig1[i] > 0)
      buckets[0][0][nsig.sig1[i]].add(nsig.id, index);
    if (nsig.sig1[i] < 0)
      buckets[0][1][-nsig.sig1[i]].add(nsig.id, index);

    if (is_grayscale(avgl))
      continue; // ignore I/Q coeff's if chrominance too low

    if (nsig.sig2[i] > 0)
      buckets[1][0][nsig.sig2[i]].add(nsig.id, index);
    if (nsig.sig2[i] < 0)
      buckets[1][1][-nsig.sig2[i]].add(nsig.id, index);

    if (nsig.sig3[i] > 0)
      buckets[2][0][nsig.sig3[i]].add(nsig.id, index);
    if (nsig.sig3[i] < 0)
      buckets[2][1][-nsig.sig3[i]].add(nsig.id, index);

#endif //FAST_POW_GEERT
  }
}

template <typename B>
inline B &dbSpaceCommon::bucket_set<B>::at(int col, int coeff, int *idxret) {
  int pn, idx;
  //TODO see if FAST_POW_GEERT gives the same results
#ifdef FAST_POW_GEERT
  pn = coeff < 0;
  idx = (coeff - pn) ^ -pn;
#else
  pn = 0;
  if (coeff > 0) {
    pn = 0;
    idx = coeff;
  } else {
    pn = 1;
    idx = -coeff;
  }
#endif
  if (idxret)
    *idxret = idx;
  return buckets[col][pn][idx];
}

template <>
void dbSpaceImpl<false>::addImageData(const ImgData *img) {
  if (hasImage(img->id)) // image already in db
    throw duplicate_id("Image already in database.");

  SigStruct *nsig = new SigStruct(get_sig_cache());
  nsig->init(img);
  write_sig_cache(nsig->cacheOfs, img);

  // insert into sigmap
  m_images.add_sig(img->id, nsig);

  imgbuckets.add(*img, m_nextIndex++);
}

template <>
void dbSpaceImpl<true>::addImageData(const ImgData *img) {
  if (hasImage(img->id)) // image already in db
    throw duplicate_id("Image already in database.");

  size_t ind = m_nextIndex++;
  if (ind > m_info.size())
    throw internal_error("Index incremented too much!");
  if (ind == m_info.size()) {
    if (ind >= m_info.capacity())
      m_info.reserve(10 + ind + ind / 40);
    m_info.resize(ind + 1);
  }
  m_info.at(ind).id = img->id;
  SigStruct::avglf2i(img->avglf, m_info[ind].avgl);
  m_info[ind].width = img->width;
  m_info[ind].height = img->height;
  m_images.add_index(img->id, ind);

  if (m_sigFile != -1) {
    size_t ofs = get_sig_cache();
    if (ofs != ind * sizeof(ImgData))
      throw internal_error("Index and cache out of sync!");
    write_sig_cache(ofs, img);
  }

  imgbuckets.add(*img, ind);
}

void dbSpaceAlter::addImageData(const ImgData *img) {
  if (m_readonly)
    throw usage_error("Not possible in imgdata mode.");
  if (hasImage(img->id)) // image already in db
    throw duplicate_id("Image already in database.");

  size_t ind;
  if (!m_deleted.empty()) {
    ind = m_deleted.back();
    m_deleted.pop_back();
  } else {
    ind = m_images.size();
    if (m_imgOff + ((off_t)ind + 1) * (off_t)sizeof(imageId) >= m_sigOff) {
      resize_header();
      if (m_imgOff + ((off_t)ind + 1) * (off_t)sizeof(imageId) >= m_sigOff)
        throw internal_error("resize_header failed!");
    }
  }

  if (!m_rewriteIDs) {
    m_f->seekp(m_imgOff + ind * sizeof(imageId));
    m_f->write(img->id);
  }
  m_f->seekp(m_sigOff + ind * sizeof(ImgData));
  m_f->write(*img);

  m_buckets.add(*img, ind);
  m_images[img->id] = ind;
}

void dbSpaceCommon::addImageBlob(imageId id, const void *blob, size_t length) {
  if (hasImage(id)) // image already in db
    throw duplicate_id("Image already in database.");

  ::image_info info;
  get_image_info((const unsigned char *)blob, length, &info);

  AutoGDImage image(resize_image_data((const unsigned char *)blob, length, NUM_PIXELS, NUM_PIXELS));

  ImgData sig;
  sigFromImage(image, id, &sig);
  return addImageData(&sig);
}

void dbSpaceCommon::imgDataFromFile(const char *filename, imageId id, ImgData *img) {
  AutoClean<mapped_file, &mapped_file::unmap> map(mapped_file(filename, false));
  AutoGDImage image(resize_image_data((const unsigned char *)map.m_base, map.m_length, NUM_PIXELS, NUM_PIXELS));
  sigFromImage(image, id, img);
}

void dbSpaceCommon::imgDataFromBlob(const void *data, size_t data_size, imageId id, ImgData *img) {
  ::image_info info;
  get_image_info((const unsigned char *)data, data_size, &info);

  AutoGDImage image(resize_image_data((const unsigned char *)data, data_size, NUM_PIXELS, NUM_PIXELS));
  sigFromImage(image, id, img);
}

void dbSpaceCommon::addImage(imageId id, const char *filename) {
  if (hasImage(id)) // image already in db
    throw duplicate_id("Image already in database.");

  ImgData sig;
  imgDataFromFile(filename, id, &sig);
  return addImageData(&sig);
}

void dbSpace::imgDataFromFile(const char *filename, imageId id, ImgData *img) {
  return dbSpaceCommon::imgDataFromFile(filename, id, img);
}

void dbSpace::imgDataFromBlob(const void *data, size_t data_size, imageId id, ImgData *img) {
  return dbSpaceCommon::imgDataFromBlob(data, data_size, id, img);
}

template <>
void dbSpaceImpl<false>::setImageRes(imageId id, int width, int height) {
  imageIterator itr = find(id);
  ImgData sig;
  read_sig_cache(itr.cOfs(), &sig);
  sig.width = width;
  sig.height = height;
  write_sig_cache(itr.cOfs(), &sig);
}

template <>
void dbSpaceImpl<true>::setImageRes(imageId id, int width, int height) {
  imageIterator itr = find(id);
  itr->width = width;
  itr->height = height;
}

void dbSpaceAlter::setImageRes(imageId id, int width, int height) {
  if (m_readonly)
    throw usage_error("Not possible in imgdata mode.");

  size_t ind = find(id)->second;
  ImgData sig = get_sig(ind);
  m_f->seekg(m_sigOff + ind * sizeof(ImgData));
  m_f->read(&sig);
  sig.width = width;
  sig.height = height;
  m_f->seekp(m_sigOff + ind * sizeof(ImgData));
  m_f->write(sig);
}

template <bool is_simple>
void dbSpaceImpl<is_simple>::load(const char *filename) {
  db_ifstream f(filename);
  DEBUG(imgdb)("Loading db... ");

  if (!f.is_open()) {
    DEBUG(warnings)("Unable to open file %s for read ops: %s.\n", filename, strerror(errno));
    return;
  }

  uint32_t v_code = f.read<uint32_t>();
  uint32_t intsizes = v_code >> 8;
  uint version = v_code & 0xff;

  if (intsizes != SRZ_V_SZ) {
    throw data_error("Cannot load database with wrong endianness or data sizes");
  } else if (version != SRZ_V0_9_0) {
    throw data_error("Database is from an unsupported version (not 0.9.0)");
  }

  count_t numImg = f.read<count_t>();
  offset_t firstOff = f.read<offset_t>();
  DEBUG_CONT(imgdb)(DEBUG_OUT, "has %" FMT_count_t " images at %llx. ", numImg, (long long)firstOff);

  // read bucket sizes and reserve space so that buckets do not
  // waste memory due to exponential growth of std::vector
  for (typename buckets_t::iterator itr = imgbuckets.begin(); itr != imgbuckets.end(); ++itr)
    itr->reserve(f.read<count_t>());
  DEBUG_CONT(imgdb)(DEBUG_OUT, "bucket sizes done at %llx... ", (long long)firstOff);

  // read IDs (for verification only)
  AutoCleanArray<imageId> ids(numImg);
  f.read(ids.ptr(), numImg);

  // read sigs
  f.seekg(firstOff);
  if (is_simple)
    m_info.resize(numImg);
  for (typename image_map::size_type k = 0; k < numImg; k++) {
    ImgData sig;
    f.read(&sig);

    size_t ind = m_nextIndex++;
    imgbuckets.add(sig, ind);

    if (ids[ind] != sig.id) {
      if (is_simple) {
        // DEBUG_CONT(imgdb)(DEBUG_OUT, "\n");
        DEBUG(warnings)("WARNING: index %zd DB header ID %08llx mismatch with sig ID %08llx.\n", ind, (long long)ids[ind], (long long)sig.id);
      } else {
        throw data_error("DB header ID mismatch with sig ID.");
      }
    }

    if (is_simple) {
      m_info[ind].id = sig.id;
      SigStruct::avglf2i(sig.avglf, m_info[ind].avgl);
      m_info[ind].width = sig.width;
      m_info[ind].height = sig.height;
      m_images.add_index(sig.id, ind);

      if (m_sigFile != -1) {
        size_t ofs = get_sig_cache();
        if (ofs != ind * sizeof(ImgData))
          throw internal_error("Index and cache out of sync!");
        write_sig_cache(ofs, &sig);
      }

    } else {
      SigStruct *nsig = new SigStruct(get_sig_cache());
      nsig->init(&sig);
      nsig->index = ind;
      write_sig_cache(nsig->cacheOfs, &sig);

      // insert new sig
      m_images.add_sig(sig.id, nsig);
    }
  }

  if (is_simple && is_disk_db)
    DEBUG_CONT(imgdb)(DEBUG_OUT, "map size: %lld... ", (long long int)lseek(imgbuckets[0][0][0].fd(), 0, SEEK_CUR));

  for (typename buckets_t::iterator itr = imgbuckets.begin(); itr != imgbuckets.end(); ++itr)
    itr->set_base();
  m_bucketsValid = true;
  DEBUG_CONT(imgdb)(DEBUG_OUT, "complete!\n");
  f.close();
}

void dbSpaceAlter::load(const char *filename) {
  m_f = new db_fstream(filename);
  m_fname = filename;
  try {
    if (!m_f->is_open()) {
      // Instead of replicating code here to create the basic file structure, we'll just make a dummy DB.
      AutoCleanPtr<dbSpace> dummy(load_file(filename, mode_normal));
      dummy->save_file(filename);
      dummy.set(NULL);
      m_f->open(filename);
      if (!m_f->is_open())
        throw io_error("Could not create DB structure.");
    }
    m_f->exceptions(std::fstream::badbit | std::fstream::failbit);

    uint32_t v_code = m_f->read<uint32_t>();
    uint version = v_code & 0xff;

    if ((v_code >> 8) == 0) {
      DEBUG(warnings)("Old database version.\n");
    } else if ((v_code >> 8) != SRZ_V_SZ) {
      throw data_error("Database incompatible with this system");
    }

    if (version != SRZ_V0_7_0 && version != SRZ_V0_9_0)
      throw data_error("Only current version is supported in alter mode, upgrade first using normal mode.");

    DEBUG(imgdb)("Loading db header (cur ver)... ");
    m_hdrOff = m_f->tellg();
    count_t numImg = m_f->read<count_t>();
    m_sigOff = m_f->read<offset_t>();

    DEBUG_CONT(imgdb)(DEBUG_OUT, "has %" FMT_count_t " images. ", numImg);
    // read bucket sizes
    for (buckets_t::iterator itr = m_buckets.begin(); itr != m_buckets.end(); ++itr)
      itr->size = m_f->read<count_t>();

    // read IDs
    m_imgOff = m_f->tellg();
    for (size_t k = 0; k < numImg; k++)
      m_images[m_f->read<count_t>()] = k;

    m_rewriteIDs = false;
    DEBUG_CONT(imgdb)(DEBUG_OUT, "complete!\n");
  } catch (const base_error &e) {
    if (m_f) {
      if (m_f->is_open())
        m_f->close();
      delete m_f;
      m_fname.clear();
    }
    DEBUG_CONT(imgdb)(DEBUG_OUT, "failed!\n");
    throw;
  }
}

static inline dbSpace *make_dbSpace(int mode) {
  return mode & dbSpaceCommon::mode_mask_alter
             ? static_cast<dbSpace *>(new dbSpaceAlter(mode & dbSpaceCommon::mode_mask_readonly))
             : mode & dbSpaceCommon::mode_mask_simple
                   ? static_cast<dbSpace *>(new dbSpaceImpl<true>(mode & dbSpaceCommon::mode_mask_readonly))
                   : static_cast<dbSpace *>(new dbSpaceImpl<false>(true));
};

dbSpace *dbSpace::load_file(const char *filename, int mode) {
  dbSpace *db = make_dbSpace(mode);
  db->load(filename);
  return db;
}

template <>
void dbSpaceImpl<false>::save_file(const char *filename) {
  /*
    Serialization order:
    [image_map::size_type] number of images
    [off_t] offset to first signature in file
    for each bucket:
    [size_t] number of images in bucket
    for each image:
    [imageId] image id at this index
    ...hole in file until offset to first signature in file, to allow adding more image ids
    then follow image signatures, see struct ImgData
  */

  DEBUG(imgdb)("Saving to %s... ", filename);
  std::string temp = std::string(filename) + ".temp";
  db_ofstream f(temp.c_str());
  if (!f.is_open())
    throw io_error(std::string("Cannot open temp file ") + temp + " for writing: " + strerror(errno));

  if (is_disk_db)
    DEBUG_CONT(imgdb)(DEBUG_OUT, "map size: %lld... ", (long long int)lseek(imgbuckets[0][0][0].fd(), 0, SEEK_CUR));
  f.write<int32_t>(SRZ_V_CODE);
  f.write<count_t>(m_images.size());
  off_t firstOff = f.tellp();
  firstOff += m_images.size() * sizeof(imageId);    // cur pos plus imageId per image
  firstOff += imgbuckets.count() * sizeof(count_t); // plus size_t per bucket

  // leave space for 1024 new IDs
  firstOff = (firstOff + 1024 * sizeof(imageId));
  DEBUG_CONT(imgdb)(DEBUG_OUT, "sig off: %llx... ", (long long int)firstOff);
  f.write<offset_t>(firstOff);

  // save bucket sizes
  for (buckets_t::iterator itr = imgbuckets.begin(); itr != imgbuckets.end(); ++itr)
    f.write<count_t>(itr->size());

  // save IDs
  for (imageIterator it = image_begin(); it != image_end(); it++)
    f.write<imageId>(it.id());

  // skip to firstOff
  f.seekp(firstOff);

  DEBUG_CONT(imgdb)(DEBUG_OUT, "sigs... ");
  // save sigs
  for (imageIterator it = image_begin(); it != image_end(); it++) {
    ImgData dsig;
    read_sig_cache(it.cOfs(), &dsig);
    f.write(dsig);
  }
  f.close();
  if (rename(temp.c_str(), filename))
    throw io_error(std::string("Cannot rename temp file ") + temp + " to DB file " + filename + ": " + strerror(errno));
  DEBUG_CONT(imgdb)(DEBUG_OUT, "done!\n");
}

template <>
void dbSpaceImpl<true>::save_file(const char *filename) { throw usage_error("Can't save read-only db."); }

struct in_deleted_tail : public std::unary_function<size_t, bool> {
  in_deleted_tail(size_t max) : m_max(max) {}
  bool operator()(size_t ind) { return ind < m_max; }
  size_t m_max;
};

// Relocate sigs from the end into the holes left by deleted images.
void dbSpaceAlter::move_deleted() {
  if (m_readonly)
    throw usage_error("Not possible in imgdata mode.");

  // need to find out which IDs are using the last few indices
  DeletedList::iterator delItr = m_deleted.begin();
  for (ImageMap::iterator itr = m_images.begin();; ++itr) {
    // Don't fill holes that are beyond the new end!
    while (delItr != m_deleted.end() && *delItr >= m_images.size())
      ++delItr;

    if (itr == m_images.end() || delItr == m_deleted.end())
      break;

    if (itr->second < m_images.size())
      continue;

    ImgData sig = get_sig(itr->second);
    itr->second = *delItr++;
    m_f->seekp(m_sigOff + itr->second * sizeof(ImgData));
    m_f->write(sig);

    if (!m_rewriteIDs) {
      m_f->seekp(m_imgOff + itr->second * sizeof(imageId));
      m_f->write(sig.id);
    }
  }
  if (delItr != m_deleted.end())
    throw data_error("Not all deleted entries purged.");

  m_deleted.clear();

  // Truncate file here? Meh, it'll be appended again soon enough anyway.
}

void dbSpaceAlter::save_file(const char *filename) {
  if (m_readonly)
    throw usage_error("Not possible in imgdata mode.");

  if (!m_f)
    return;
  if (filename && m_fname != filename)
    throw param_error("Cannot save to different filename.");

  DEBUG(imgdb)("saving file, %zd deleted images... ", m_deleted.size());
  if (!m_deleted.empty())
    move_deleted();

  if (m_rewriteIDs) {
    DEBUG_CONT(imgdb)(DEBUG_OUT, "Rewriting all IDs... ");
    imageId_list ids(m_images.size(), ~imageId());
    for (ImageMap::iterator itr = m_images.begin(); itr != m_images.end(); ++itr) {
      if (itr->second >= m_images.size())
        throw data_error("Invalid index on save.");
      if (ids[itr->second] != ~imageId())
        throw data_error("Duplicate index on save.");
      ids[itr->second] = itr->first;
    }
    // Shouldn't be possible.
    if (ids.size() != m_images.size())
      throw data_error("Image indices do not match images.");

    m_f->seekp(m_imgOff);
    m_f->write(&ids.front(), ids.size());
    m_rewriteIDs = false;
  }

  DEBUG_CONT(imgdb)(DEBUG_OUT, "saving header... ");
  m_f->seekp(0);
  m_f->write<uint32_t>(SRZ_V_CODE);
  m_f->seekp(m_hdrOff);
  m_f->write<count_t>(m_images.size());
  m_f->write(m_sigOff);
  m_f->write(m_buckets);

  DEBUG_CONT(imgdb)(DEBUG_OUT, "done!\n");
  m_f->flush();
}

// Need more space for image IDs in the header. Relocate first few
// image signatures to the end of the file and use the freed space
// for new image IDs until we run out of space again.
void dbSpaceAlter::resize_header() {
  if (m_readonly)
    throw usage_error("Not possible in imgdata mode.");

  // make space for 1024 new IDs
  size_t numrel = (1024 * sizeof(imageId) + sizeof(ImgData) - 1) / sizeof(ImgData);
  DEBUG(imgdb)("relocating %zd/%zd images... from %llx ", numrel, m_images.size(), (long long int)m_sigOff);
  if (m_images.size() < numrel)
    throw internal_error("dbSpaceAlter::resize_header called with too few images!");
  ImgData sigs[numrel];
  m_f->seekg(m_sigOff);
  m_f->read(sigs, numrel);
  off_t writeOff = m_sigOff + m_images.size() * sizeof(ImgData);
  m_sigOff = m_f->tellg();
  DEBUG_CONT(imgdb)(DEBUG_OUT, "to %llx (new off %llx) ", (long long int)writeOff, (long long int)m_sigOff);
  m_f->seekp(writeOff);
  m_f->write(sigs, numrel);

  size_t addrel = m_images.size() - numrel;
  for (ImageMap::iterator itr = m_images.begin(); itr != m_images.end(); ++itr)
    itr->second = (itr->second >= numrel ? itr->second - numrel : itr->second + addrel);
  DEBUG_CONT(imgdb)(DEBUG_OUT, "done.\n");

  m_rewriteIDs = true;
}

template <bool is_simple>
struct sim_result : public index_iterator<is_simple>::base_type {
  typedef typename index_iterator<is_simple>::base_type itr_type;
  sim_result(Score s, const itr_type &i) : itr_type(i), score(s) {}
  bool operator<(const sim_result &other) const { return score < other.score; }
  Score score;
};

template <bool is_simple>
inline bool dbSpaceImpl<is_simple>::skip_image(const imageIterator &itr, const queryArg &query) {
  return (is_simple && !itr.avgl().v[0]) ||
         ((query.flags & flag_mask) && ((itr.mask() & query.mask_and) != query.mask_xor));
}

template <bool is_simple>
template <int num_colors>
sim_vector dbSpaceImpl<is_simple>::do_query(queryArg q) {
  int c;
  Score scale = 0;
  int sketch = q.flags & flag_sketch ? 1 : 0;
  //fprintf(stderr, is_simple?"In do_query<true%d>.\n":"In do_query<false%d>.\n", num_colors);

  if (!m_bucketsValid)
    throw usage_error("Can't query with invalid buckets.");

  size_t count = m_nextIndex;
  AutoCleanArray<Score> scores(count);

  // Luminance score (DC coefficient).
  for (imageIterator itr = image_begin(); itr != image_end(); ++itr) {
    Score s = 0;
#ifdef DEBUG_QUERY
    fprintf(stderr, "%zd:", itr.index());
#endif
    for (c = 0; c < num_colors; c++) {
#ifdef DEBUG_QUERY
      Score o = s;
      fprintf(stderr, " %d=%f*abs(%f-%f)", c, ScD(weights[sketch][0][c]), ScD(itr.avgl().v[c]), ScD(q.avgl.v[c]));
#endif
      s += DScSc(((DScore)weights[sketch][0][c]) * std::abs(itr.avgl().v[c] - q.avgl.v[c]));
#ifdef DEBUG_QUERY
      fprintf(stderr, "=%f", ScD(s - o));
#endif
    }
    scores[itr.index()] = s;
  }

#ifdef DEBUG_QUERY
  fprintf(stderr, "Lumi scores:");
  for (int i = 0; i < count; i++)
    fprintf(stderr, " %d=%f", i, ScD(scores[i]));
  fprintf(stderr, "\n");
#endif
#if QUERYSTATS
  size_t coefcnt = 0, coeflen = 0, coefmax = 0;
  size_t setcnt[NUM_COEFS * num_colors];
  AutoCleanArray<uint8_t> counts(count);
  memset(counts.ptr(), 0, sizeof(counts[0]) * count);
  memset(setcnt, 0, sizeof(setcnt));
#endif
  for (int b = (q.flags & flag_fast) ? NUM_COEFS : 0; b < NUM_COEFS; b++) { // for every coef on a sig
    for (c = 0; c < num_colors; c++) {
      int idx;
      bucket_type &bucket = imgbuckets.at(c, q.sig[c][b], &idx);
      if (bucket.empty())
        continue;
      if (q.flags & flag_nocommon && bucket.size() > count / 10)
        continue;

      Score weight = weights[sketch][imgBin[idx]][c];
      //fprintf(stderr, "%d:%d=%d has %zd=%f\n", b, c, idx, bucket.size(), ScD(weight));
      scale -= weight;

      // update the score of every image which has this coef
      AutoImageIdIndex_map<is_simple> map(bucket.map_all(false));
#if QUERYSTATS
      size_t len = bucket.size();
      coeflen += len;
      coefmax = std::max(coefmax, len);
      coefcnt++;
#endif
      for (idIndexIterator itr(map.begin(), *this); itr != map.end(); ++itr) {
        scores[itr.index()] -= weight;
#if QUERYSTATS
        counts[itr.index()]++;
#endif
      }
      for (idIndexTailIterator itr(bucket.tail().begin(), *this); itr != bucket.tail().end(); ++itr) {
        scores[itr.index()] -= weight;
#if QUERYSTATS
        counts[itr.index()]++;
#endif
      }
    }
  }
//fprintf(stderr, "Total scale=%f\n", ScD(scale));
#ifdef DEBUG_QUERY
  fprintf(stderr, "Final scores:");
  for (int i = 0; i < count; i++)
    fprintf(stderr, " %d=%f", i, ScD(scores[i]));
  fprintf(stderr, "\n");
#endif

  typedef std::priority_queue<sim_result<is_simple>> sigPriorityQueue;

  sigPriorityQueue pqResults; /* results priority queue; largest at top */

  imageIterator itr = image_begin();

  sim_vector V;

  typedef std::map<int, size_t> set_map;
  set_map sets;
  unsigned int need = q.numres;

  // Fill up the numres-bounded priority queue (largest at top):
  while (pqResults.size() < need && itr != image_end()) {
    //fprintf(stderr, "ID %08lx mask %x qflm %d %x -> %x = %x?\n", itr.id(), itr.mask(), q.flags & flag_mask, q.mask_and, itr.mask() & q.mask_and,q.mask_xor);
    if (skip_image(itr, q)) {
      ++itr;
      continue;
    }

#if QUERYSTATS
    setcnt[counts[itr.index()]]++;
#endif
    pqResults.push(sim_result<is_simple>(scores[itr.index()], itr));

    if (q.flags & flag_uniqueset) //{
      need += ++sets[itr.set()] > 1;
    //imageIterator top(pqResults.top(), *this);fprintf(stderr, "Added id=%08lx score=%.2f set=%x, now need %d. Worst is id %08lx score %.2f set %x has %zd\n", itr.id(), (double)scores[itr.index()]/ScoreMax, itr.set(), need, top.id(), (double)pqResults.top().score/ScoreMax, top.set(), sets[top.set()]); }
    ++itr;
  }

  for (; itr != image_end(); ++itr) {
    // only consider if not ignored due to keywords and if is a better match than the current worst match
#if QUERYSTATS
    if (!skip_image(itr, q))
      setcnt[counts[itr.index()]]++;
#endif
    if (scores[itr.index()] < pqResults.top().score) {
      //fprintf(stderr, "ID %08lx mask %x qflm %d %x -> %x = %x?\n", itr.id(), itr.mask(), q.flags & flag_mask, q.mask_and, itr.mask() & q.mask_and,q.mask_xor);
      if (skip_image(itr, q))
        continue;

      // Make room by dropping largest entry:
      if (q.flags & flag_uniqueset) {
        pqResults.push(sim_result<is_simple>(scores[itr.index()], itr));
        need += ++sets[itr.set()] > 1;
        //imageIterator top(pqResults.top(), *this);fprintf(stderr, "Added id=%08lx score=%.2f set=%x, now need %d. Worst is id %08lx score %.2f set %x has %zd\n", itr.id(), (double)scores[itr.index()]/ScoreMax, itr.set(), need, top.id(), (double)pqResults.top().score/ScoreMax, top.set(), sets[top.set()]);

        while (pqResults.size() > need || sets[imageIterator(pqResults.top(), *this).set()] > 1) {
          need -= sets[imageIterator(pqResults.top(), *this).set()]-- > 1;
          pqResults.pop();
          //imageIterator top(pqResults.top(), *this);fprintf(stderr, "Dropped worst, now need %d. New worst is id %08lx score %.2f set %x has %zd\n", need, top.id(), (double)pqResults.top().score/ScoreMax, top.set(), sets[top.set()]);
        }
      } else {
        pqResults.pop();
        // Insert new entry:
        pqResults.push(sim_result<is_simple>(scores[itr.index()], itr));
      }
    }
  }

  //fprintf(stderr, "Have %zd images in result set.\n", pqResults.size());
  if (scale != 0)
    scale = ((DScore)MakeScore(1)) * MakeScore(1) / scale;
  //fprintf(stderr, "Inverted scale=%f\n", ScD(scale));
  while (!pqResults.empty()) {
    const sim_result<is_simple> &curResTmp = pqResults.top();

    imageIterator itr(curResTmp, *this);
    //fprintf(stderr, "Candidate %08lx = %.2f, set %x has %zd.\n", itr.id(), ScD(curResTmp.score), itr.set(), sets[itr.set()]);
    if (!(q.flags & flag_uniqueset) || sets[itr.set()]-- < 2)
      V.push_back(sim_value(itr.id(), DScSc(((DScore)curResTmp.score) * 100 * scale), itr.width(), itr.height()));
    //else fprintf(stderr, "Skipped!\n");
    pqResults.pop();
  }

#if QUERYSTATS
  size_t num = 0;
  for (size_t i = 0; i < sizeof(setcnt) / sizeof(setcnt[0]); i++)
    num += setcnt[i];
  DEBUG(imgdb)("Query complete, coefcnt=%zd coeflen=%zd coefmax=%zd numset=%zd/%zd\nCounts: ", coefcnt, coeflen, coefmax, num, m_images.size());
  num = 0;
  for (size_t i = sizeof(setcnt) / sizeof(setcnt[0]) - 1; i > 0 && num < 10; i--)
    if (setcnt[i]) {
      num++;
      DEBUG_CONT(imgdb)(DEBUG_OUT, "%zd=%zd; ", i, setcnt[i]);
    }
  DEBUG_CONT(imgdb)(DEBUG_OUT, "\n");
#endif
  std::reverse(V.begin(), V.end());
  //fprintf(stderr, "Returning %zd images, top score %f.\n", V.size(), ScD(V[0].score));
  return V;
}

template <bool is_simple>
inline sim_vector
dbSpaceImpl<is_simple>::queryImg(const queryArg &query) {
  if ((query.flags & flag_grayscale) || is_grayscale(query.avgl))
    return do_query<1>(query);
  else
    return do_query<3>(query);
}

// cluster by similarity. Returns list of list of imageIds (img ids)
/*
imageId_list_2 clusterSim(const int dbId, float thresd, int fast = 0) {
	imageId_list_2 res;		// will hold a list of lists. ie. a list of clusters
	sigMap wSigs(dbSpace[dbId]->sigs);		// temporary map of sigs, as soon as an image becomes part of a cluster, it's removed from this map
	sigMap wSigsTrack(dbSpace[dbId]->sigs);	// temporary map of sigs, as soon as an image becomes part of a cluster, it's removed from this map

	for (sigIterator sit = wSigs.begin(); sit != wSigs.end(); sit++) {	// for every img on db
		imageId_list res2;

		if (fast) {
			res2 =
				queryImgDataForThresFast(&wSigs, (*sit).second->avgl,
				thresd, 1);
		} else {
			res2 =
				queryImgDataForThres(dbId, &wSigs, (*sit).second->sig1,
				(*sit).second->sig2,
				(*sit).second->sig3,
				(*sit).second->avgl, thresd, 1);
		}
		//    continue;
		imageId hid = (*sit).second->id;
		//    if () 
		wSigs.erase(hid);
		if (res2.size() <= 1) {
			if (wSigs.size() <= 1)
				break;		// everything already added to a cluster sim. Bail out immediately, otherwise next iteration 
			// will segfault when incrementing sit
			continue;
		}
		res2.push_front(hid);
		res.push_back(res2);
		if (wSigs.size() <= 1)
			break;
		// sigIterator sit2 = wSigs.end();
		//    sigIterator sit3 = sit++;
	}
	return res;
}
 */

template <>
void dbSpaceImpl<false>::removeImage(imageId id) {
  SigStruct *isig = find(id).sig();
  ImgData nsig;
  read_sig_cache(isig->cacheOfs, &nsig);
  imgbuckets.remove(nsig);
  m_bucketsValid = false;
  m_images.erase(id);
  delete isig;
}

template <>
void dbSpaceImpl<true>::removeImage(imageId id) {
  // Can't efficiently remove it from buckets, just mark it as
  // invalid and remove it from query results.
  m_info[find(id).index()].avgl.v[0] = 0;
  m_images.erase(id);
}

void dbSpaceAlter::removeImage(imageId id) {
  if (m_readonly)
    throw usage_error("Not possible in imgdata mode.");

  ImageMap::iterator itr = find(id);
  m_deleted.push_back(itr->second);
  m_images.erase(itr);
}

template <typename B>
inline void dbSpaceCommon::bucket_set<B>::remove(const ImgData &nsig) {
  lumin_native avgl;
  SigStruct::avglf2i(nsig.avglf, avgl);
  for (int i = 0; 0 && i < NUM_COEFS; i++) {
    if (nsig.sig1[i] > 0)
      buckets[0][0][nsig.sig1[i]].remove(nsig.id);
    if (nsig.sig1[i] < 0)
      buckets[0][1][-nsig.sig1[i]].remove(nsig.id);

    if (is_grayscale(avgl))
      continue; // ignore I/Q coeff's if chrominance too low

    if (nsig.sig2[i] > 0)
      buckets[1][0][nsig.sig2[i]].remove(nsig.id);
    if (nsig.sig2[i] < 0)
      buckets[1][1][-nsig.sig2[i]].remove(nsig.id);

    if (nsig.sig3[i] > 0)
      buckets[2][0][nsig.sig3[i]].remove(nsig.id);
    if (nsig.sig3[i] < 0)
      buckets[2][1][-nsig.sig3[i]].remove(nsig.id);
  }
}

Score dbSpaceCommon::calcAvglDiff(imageId id1, imageId id2) {
  /* return the average luminance difference */

  // are images on db ?
  lumin_native avgl1, avgl2;
  getImgAvgl(id1, avgl1);
  getImgAvgl(id2, avgl2);
  return std::abs(avgl1.v[0] - avgl2.v[0]) + std::abs(avgl1.v[1] - avgl2.v[1]) + std::abs(avgl1.v[2] - avgl2.v[2]);
}

Score dbSpaceCommon::calcSim(imageId id1, imageId id2, bool ignore_color) {
  /* use it to tell the content-based difference between two images */
  ImgData dsig1, dsig2;
  getImgDataByID(id1, &dsig1);
  getImgDataByID(id2, &dsig2);

  Idx *const sig1[3] = {dsig1.sig1, dsig1.sig2, dsig1.sig3};
  Idx *const sig2[3] = {dsig2.sig1, dsig2.sig2, dsig2.sig3};

  Score score = 0, scale = 0;
  lumin_native avgl1, avgl2;
  image_info::avglf2i(dsig1.avglf, avgl1);
  image_info::avglf2i(dsig2.avglf, avgl2);

  int cnum = ignore_color || is_grayscale(avgl1) || is_grayscale(avgl2) ? 1 : 3;

  for (int c = 0; c < cnum; c++)
    score += DScSc(2 * ((DScore)weights[0][0][c]) * std::abs(avgl1.v[c] - avgl2.v[c]));

  for (int c = 0; c < cnum; c++) {
    std::sort(sig1[c] + 0, sig1[c] + NUM_COEFS);
    std::sort(sig2[c] + 0, sig2[c] + NUM_COEFS);

    for (int b1 = 0, b2 = 0; b1 < NUM_COEFS || b2 < NUM_COEFS;) {
      int ind1 = b1 == NUM_COEFS ? std::numeric_limits<int>::max() : sig1[c][b1];
      int ind2 = b2 == NUM_COEFS ? std::numeric_limits<int>::max() : sig2[c][b2];

      Score weight = weights[0][imgBin[std::abs(ind1 < ind2 ? ind1 : ind2)]][c];
      scale -= weight;

      if (ind1 == ind2)
        score -= weight;

      b1 += ind1 <= ind2;
      b2 += ind2 <= ind1;
    }
  }

  scale = ((DScore)MakeScore(1)) * MakeScore(1) / scale;
  return DScSc(((DScore)score) * 100 * scale);
}

template <>
void dbSpaceImpl<false>::rehash() {
  for (buckets_t::iterator itr = imgbuckets.begin(); itr != imgbuckets.end(); ++itr) {
    size_t size = itr->size();
    itr->clear();
    itr->resize(size & ~pageImgMask);
  }

  for (imageIterator itr = image_begin(); itr != image_end(); ++itr) {
    ImgData dsig;
    read_sig_cache(itr.cOfs(), &dsig);
    imgbuckets.add(dsig, itr.index());
  }

  m_bucketsValid = true;
}

template <>
void dbSpaceImpl<true>::rehash() { throw usage_error("Invalid for read-only db."); }

void dbSpaceAlter::rehash() {
  if (m_readonly)
    throw usage_error("Not possible in imgdata mode.");

  memset(m_buckets.begin(), 0, m_buckets.size());

  for (ImageMap::iterator it = m_images.begin(); it != m_images.end(); ++it)
    m_buckets.add(get_sig(it->second), it->second);
}

template <bool is_simple>
void dbSpaceImpl<is_simple>::getImgQueryArg(imageId id, queryArg *query) {
  ImgData img = get_sig_from_cache(id);
  queryFromImgData(img, query);
}

template <bool is_simple>
size_t dbSpaceImpl<is_simple>::getImgCount() {
  return m_images.size();
}

size_t dbSpaceAlter::getImgCount() {
  return m_images.size();
}

template <bool is_simple>
stats_t dbSpaceImpl<is_simple>::getCoeffStats() {
  stats_t ret;
  ret.reserve(imgbuckets.count());

  for (typename buckets_t::iterator itr = imgbuckets.begin(); itr != imgbuckets.end(); ++itr) {
    ret.push_back(std::make_pair(itr - imgbuckets.begin(), itr->size()));
  }

  return ret;
}

template <bool is_simple>
imageId_list dbSpaceImpl<is_simple>::getImgIdList() {
  imageId_list ids;

  ids.reserve(getImgCount());
  for (imageIterator it = image_begin(); it != image_end(); ++it)
    if (!is_simple || it.avgl().v[0] != 0)
      ids.push_back(it.id());

  return ids;
}

imageId_list dbSpaceAlter::getImgIdList() {
  imageId_list ids;

  ids.reserve(m_images.size());
  for (ImageMap::iterator it = m_images.begin(); it != m_images.end(); ++it)
    ids.push_back(it->first);

  return ids;
}

template <>
image_info_list dbSpaceImpl<false>::getImgInfoList() {
  image_info_list info;

  // TODO is there a faster way for getting a maps key list and returning a vector from it ?
  for (imageIterator it = image_begin(); it != image_end(); ++it) {
    info.push_back(image_info(it.id(), it.avgl(), it.width(), it.height()));
  }

  return info;
}
template <>
image_info_list dbSpaceImpl<true>::getImgInfoList() { return m_info; }

dbSpace::dbSpace() {}
dbSpace::~dbSpace() {}

template <bool is_simple>
dbSpaceImpl<is_simple>::dbSpaceImpl(bool with_struct) : m_sigFile(-1),
                                                        m_cacheOfs(0),
                                                        m_nextIndex(0),
                                                        m_bucketsValid(true) {

  if (!imgBinInited)
    initImgBin();
  if (imgbuckets.count() != sizeof(imgbuckets) / sizeof(imgbuckets[0][0][0]))
    throw internal_error("bucket_set.count() is wrong!");

  if (with_struct)
    m_sigFile = tempfile();
}

dbSpaceAlter::dbSpaceAlter(bool readonly) : m_f(NULL), m_readonly(readonly) {
  if (!imgBinInited)
    initImgBin();
}

template <>
dbSpaceImpl<false>::~dbSpaceImpl() {
  close(m_sigFile);
  for (imageIterator itr = image_begin(); itr != image_end(); ++itr)
    delete itr.sig();
}

template <>
dbSpaceImpl<true>::~dbSpaceImpl() {
  if (m_sigFile != -1)
    close(m_sigFile);
}

dbSpaceAlter::~dbSpaceAlter() {
  if (m_f && !m_readonly) {
    save_file(NULL);
    m_f->close();
    delete m_f;
    m_f = NULL;
    m_fname.clear();
  }
}

template <bool is_simple>
size_t dbSpaceImpl<is_simple>::get_sig_cache() {
  size_t ofs = m_cacheOfs;
  m_cacheOfs += sizeof(ImgData);
  return ofs;
}

template <bool is_simple>
void dbSpaceImpl<is_simple>::read_sig_cache(size_t ofs, ImgData *sig) {
  if (m_sigFile == -1)
    throw internal_error("Can't read sig cache when using simple db.");
  if (lseek(m_sigFile, ofs, SEEK_SET) == -1)
    throw io_error("Can't seek in sig cache.");
  if (read(m_sigFile, sig, sizeof(ImgData)) != sizeof(ImgData))
    throw io_error("Can't read sig cache.");
}

template <bool is_simple>
void dbSpaceImpl<is_simple>::write_sig_cache(size_t ofs, const ImgData *sig) {
  if (m_sigFile == -1)
    throw internal_error("Can't write sig cache when using simple db.");
  if (lseek(m_sigFile, ofs, SEEK_SET) == -1)
    throw io_error("Can't seek in sig cache.");
  if (write(m_sigFile, sig, sizeof(ImgData)) != sizeof(ImgData))
    throw io_error("Can't write to sig cache.");
}

} // namespace imgdb
