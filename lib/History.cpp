/*
    This file is part of Konsole, an X terminal.
    Copyright 1997,1998 by Lars Doelle <lars.doelle@on-line.de>

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
    Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA
    02110-1301  USA.
*/

// Own
#include "History.h"

// System
#include <algorithm>
#include <iostream>
#include <cstdlib>
#include <cstdio>
#include <sys/types.h>
#include <cerrno>

#ifndef Q_OS_WIN
#include <unistd.h>
#include <sys/mman.h>
#endif

#include <QtDebug>

// KDE
//#include <kde_file.h>
//#include <kdebug.h>

// Reasonable line size
#define LINE_SIZE    1024
#define KDE_lseek lseek

using namespace Konsole;

/*
   An arbitrary long scroll.

   One can modify the scroll only by adding either cells
   or newlines, but access it randomly.

   The model is that of an arbitrary wide typewriter scroll
   in that the scroll is a serie of lines and each line is
   a serie of cells with no overwriting permitted.

   The implementation provides arbitrary length and numbers
   of cells and line/column indexed read access to the scroll
   at constant costs.

KDE4: Can we use QTemporaryFile here, instead of KTempFile?

FIXME: some complain about the history buffer consuming the
       memory of their machines. This problem is critical
       since the history does not behave gracefully in cases
       where the memory is used up completely.

       I put in a workaround that should handle it problem
       now gracefully. I'm not satisfied with the solution.

FIXME: Terminating the history is not properly indicated
       in the menu. We should throw a signal.

FIXME: There is noticeable decrease in speed, also. Perhaps,
       there whole feature needs to be revisited therefore.
       Disadvantage of a more elaborated, say block-oriented
       scheme with wrap around would be it's complexity.
*/

//FIXME: temporary replacement for tmpfile
//       this is here one for debugging purpose.

//#define tmpfile xTmpFile

// History File ///////////////////////////////////////////

/*
  A Row(X) data type which allows adding elements to the end.
*/

HistoryFile::HistoryFile()
    : length(0)
    , fileMap(nullptr)
    , readWriteBalance(0)
{
    if (tmpFile.open()) {
#if defined(Q_OS_LINUX)
// TODO:
        // qWarning(KonsoleDebug, "HistoryFile: /proc/%lld/fd/%d", qApp->applicationPid(), _tmpFile.handle());
#endif
        // On some systems QTemporaryFile creates unnamed file.
        // Do not interfere in such cases.
        if (tmpFile.exists()) {
            // Remove file entry from filesystem. Since the file
            // is opened, it will still be available for reading
            // and writing. This guarantees the file won't remain
            // in filesystem after process termination, even when
            // there was a crash.
#ifndef Q_OS_WIN
            unlink(QFile::encodeName(tmpFile.fileName()).constData());
#else
            // TODO Windows
#endif
        }
    }
}

HistoryFile::~HistoryFile()
{
    if (fileMap)
        unmap();
}

//TODO:  Mapping the entire file in will cause problems if the history file becomes exceedingly large,
//(ie. larger than available memory).  HistoryFile::map() should only map in sections of the file at a time,
//to avoid this.
void HistoryFile::map()
{
    Q_ASSERT(fileMap == nullptr);

    if (tmpFile.flush()) {
        Q_ASSERT(tmpFile.size() >= length);
        fileMap = tmpFile.map(0, length);
    }

    // if mmap'ing fails, fall back to the read-lseek combination
    if (fileMap == nullptr) {
        readWriteBalance = 0;
        qWarning() << "mmap'ing history failed.  errno = " << errno;
    }
}

void HistoryFile::unmap()
{
    Q_ASSERT(fileMap);

    if (tmpFile.unmap(fileMap))
        fileMap = nullptr;

    Q_ASSERT(fileMap == nullptr);
}

bool HistoryFile::isMapped() const
{
    return (fileMap != nullptr);
}

void HistoryFile::add(const char* bytes, qint64 len)
{
    if (fileMap != nullptr)
        unmap();

    if (readWriteBalance < INT_MAX)
        readWriteBalance++;

    qint64 rc = 0;

    if (!tmpFile.seek(length)) {
        perror("HistoryFile::add.seek");
        return;
    }
    rc = tmpFile.write(bytes, len);
    if (rc < 0) {
        perror("HistoryFile::add.write");
        return;
    }
    length += rc;
}

void HistoryFile::get(char* bytes, int len, int loc)
{
    if (loc < 0 || len < 0 || loc + len > (qint64)(length * sizeof(LineProperty))) {
        fprintf(stderr, "getHist(...,%d,%d): invalid args.\n", len, loc);
        return;
    }

    // count number of get() calls vs. number of add() calls.
    // If there are many more get() calls compared with add()
    // calls (decided by using MAP_THRESHOLD) then mmap the log
    // file to improve performance.
    if (readWriteBalance > INT_MIN)
        readWriteBalance--;
    if ((fileMap == nullptr) && readWriteBalance < MAP_THRESHOLD)
        map();

    if (fileMap != nullptr)
        memcpy(bytes, fileMap + loc, len);
    else {
        qint64 rc = 0;

        if (!tmpFile.seek(loc)) {
            perror("HistoryFile::get.seek");
            return;
        }
        rc = tmpFile.read(bytes, len);
        if (rc < 0) {
            perror("HistoryFile::get.read");
            return;
        }
    }
}

qint64 HistoryFile::len() const
{
  return length;
}


// History Scroll abstract base class //////////////////////////////////////


HistoryScroll::HistoryScroll(HistoryType* t)
  : m_histType(t)
{
}

HistoryScroll::~HistoryScroll()
{
  delete m_histType;
}

bool HistoryScroll::hasScroll() const
{
  return true;
}

// History Scroll File //////////////////////////////////////

/*
   The history scroll makes a Row(Row(Cell)) from
   two history buffers. The index buffer contains
   start of line positions which refers to the cells
   buffer.

   Note that index[0] addresses the second line
   (line #1), while the first line (line #0) starts
   at 0 in cells.
*/

HistoryScrollFile::HistoryScrollFile(const QString &logFileName)
  : HistoryScroll(new HistoryTypeFile(logFileName)),
  m_logFileName(logFileName)
{
}

HistoryScrollFile::~HistoryScrollFile()
{
}

int HistoryScrollFile::getLines() const
{
  return index.len() / sizeof(int);
}

int HistoryScrollFile::getLineLen(int lineno) const
{
  return (startOfLine(lineno+1) - startOfLine(lineno)) / sizeof(Character);
}

bool HistoryScrollFile::isWrappedLine(int lineno) const
{
  if (lineno>=0 && lineno <= getLines()) {
    unsigned char flag;
    lineflags.get((char*)&flag, sizeof(unsigned char), (lineno) * sizeof(unsigned char));
    return flag;
  }
  return false;
}

int HistoryScrollFile::startOfLine(int lineno) const
{
  if (lineno <= 0) return 0;
  if (lineno <= getLines())
    {

    if (!index.isMapped())
            index.map();

    int res = 0;
    index.get((char*)&res, sizeof(int), (lineno - 1) * sizeof(int));
    return res;
    }
  return cells.len();
}

void HistoryScrollFile::getCells(int lineno, int colno, int count, Character res[]) const
{
  cells.get((char*)res, count * sizeof(Character), startOfLine(lineno) + colno * sizeof(Character));
}

void HistoryScrollFile::addCells(const Character text[], int count)
{
  cells.add((char*)text, count * sizeof(Character));
}

void HistoryScrollFile::addLine(bool previousWrapped)
{
  if (index.isMapped())
          index.unmap();

  int locn = cells.len();
  index.add((char*)&locn,sizeof(int));
  unsigned char flags = previousWrapped ? 0x01 : 0x00;
  lineflags.add((char*)&flags, sizeof(unsigned char));
}


// History Scroll Buffer //////////////////////////////////////
HistoryScrollBuffer::HistoryScrollBuffer(unsigned int maxLineCount)
  : HistoryScroll(new HistoryTypeBuffer(maxLineCount))
   ,_historyBuffer()
   ,_maxLineCount(0)
   ,_usedLines(0)
   ,_head(0)
{
  setMaxNbLines(maxLineCount);
}

HistoryScrollBuffer::~HistoryScrollBuffer()
{
    delete[] _historyBuffer;
}

void HistoryScrollBuffer::addCellsVector(const QVector<Character>& cells)
{
    _head++;
    if ( _usedLines < _maxLineCount )
        _usedLines++;

    if ( _head >= _maxLineCount )
    {
        _head = 0;
    }

    _historyBuffer[bufferIndex(_usedLines-1)] = cells;
    _wrappedLine[bufferIndex(_usedLines-1)] = false;
}
void HistoryScrollBuffer::addCells(const Character a[], int count)
{
  HistoryLine newLine(count);
  std::copy(a,a+count,newLine.begin());

  addCellsVector(newLine);
}

void HistoryScrollBuffer::addLine(bool previousWrapped)
{
    _wrappedLine[bufferIndex(_usedLines-1)] = previousWrapped;
}

int HistoryScrollBuffer::getLines() const
{
    return _usedLines;
}

int HistoryScrollBuffer::getLineLen(int lineNumber) const
{
  Q_ASSERT( lineNumber >= 0 && lineNumber < _maxLineCount );

  if ( lineNumber < _usedLines )
  {
    return _historyBuffer[bufferIndex(lineNumber)].size();
  }
  else
  {
    return 0;
  }
}

bool HistoryScrollBuffer::isWrappedLine(int lineNumber) const
{
  Q_ASSERT( lineNumber >= 0 && lineNumber < _maxLineCount );

  if (lineNumber < _usedLines)
  {
    //kDebug() << "Line" << lineNumber << "wrapped is" << _wrappedLine[bufferIndex(lineNumber)];
    return _wrappedLine[bufferIndex(lineNumber)];
  }
  else
    return false;
}

void HistoryScrollBuffer::getCells(int lineNumber, int startColumn, int count, Character buffer[]) const
{
  if ( count == 0 ) return;

  Q_ASSERT( lineNumber < _maxLineCount );

  if (lineNumber >= _usedLines)
  {
    memset(static_cast<void*>(buffer), 0, count * sizeof(Character));
    return;
  }

  const HistoryLine& line = _historyBuffer[bufferIndex(lineNumber)];

  //kDebug() << "startCol " << startColumn;
  //kDebug() << "line.size() " << line.size();
  //kDebug() << "count " << count;

  Q_ASSERT( startColumn <= line.size() - count );

  memcpy(buffer, line.constData() + startColumn , count * sizeof(Character));
}

void HistoryScrollBuffer::setMaxNbLines(unsigned int lineCount)
{
    HistoryLine* oldBuffer = _historyBuffer;
    HistoryLine* newBuffer = new HistoryLine[lineCount];

    for ( int i = 0 ; i < qMin(_usedLines,static_cast<int>(lineCount)) ; i++ )
    {
        newBuffer[i] = oldBuffer[bufferIndex(i)];
    }

    _usedLines = qMin(_usedLines,static_cast<int>(lineCount));
    _maxLineCount = lineCount;
    _head = ( _usedLines == _maxLineCount ) ? 0 : _usedLines-1;

    _historyBuffer = newBuffer;
    delete[] oldBuffer;

    _wrappedLine.resize(lineCount);
    dynamic_cast<HistoryTypeBuffer*>(m_histType)->m_nbLines = lineCount;
}

int HistoryScrollBuffer::bufferIndex(int lineNumber) const
{
    Q_ASSERT( lineNumber >= 0 );
    Q_ASSERT( lineNumber < _maxLineCount );
    Q_ASSERT( (_usedLines == _maxLineCount) || lineNumber <= _head );

    if ( _usedLines == _maxLineCount )
    {
        return (_head+lineNumber+1) % _maxLineCount;
    }
    else
    {
        return lineNumber;
    }
}


// History Scroll None //////////////////////////////////////

HistoryScrollNone::HistoryScrollNone()
  : HistoryScroll(new HistoryTypeNone())
{
}

HistoryScrollNone::~HistoryScrollNone()
{
}

bool HistoryScrollNone::hasScroll() const
{
  return false;
}

int  HistoryScrollNone::getLines() const
{
  return 0;
}

int  HistoryScrollNone::getLineLen(int) const
{
  return 0;
}

bool HistoryScrollNone::isWrappedLine(int /*lineno*/) const
{
  return false;
}

void HistoryScrollNone::getCells(int, int, int, Character []) const
{
}

void HistoryScrollNone::addCells(const Character [], int)
{
}

void HistoryScrollNone::addLine(bool)
{
}

#ifndef Q_OS_WIN

// History Scroll BlockArray //////////////////////////////////////
HistoryScrollBlockArray::HistoryScrollBlockArray(size_t size)
  : HistoryScroll(new HistoryTypeBlockArray(size))
{
  m_blockArray.setHistorySize(size); // nb. of lines.
}

HistoryScrollBlockArray::~HistoryScrollBlockArray()
{
}

int  HistoryScrollBlockArray::getLines() const
{
  return m_lineLengths.count();
}

int HistoryScrollBlockArray::getLineLen(int lineno) const
{
    if ( m_lineLengths.contains(lineno) )
        return m_lineLengths[lineno];
    else
        return 0;
}

bool HistoryScrollBlockArray::isWrappedLine(int /*lineno*/) const
{
  return false;
}

void HistoryScrollBlockArray::getCells(int lineno, int colno,
                                       int count, Character res[]) const
{
  if (!count) return;

  const Block *b = m_blockArray.at(lineno);

  if (!b) {
    memset(static_cast<void*>(res), 0, count * sizeof(Character)); // still better than random data
    return;
  }

  Q_ASSERT(((colno + count) * sizeof(Character)) < ENTRIES);
  memcpy(res, b->data + (colno * sizeof(Character)), count * sizeof(Character));
}

void HistoryScrollBlockArray::addCells(const Character a[], int count)
{
  Block *b = m_blockArray.lastBlock();

  if (!b) return;

  // put cells in block's data
  Q_ASSERT((count * sizeof(Character)) < ENTRIES);

  memset(b->data, 0, sizeof(b->data));

  memcpy(b->data, a, count * sizeof(Character));
  b->size = count * sizeof(Character);

  size_t res = m_blockArray.newBlock();
  Q_ASSERT(res > 0);
  Q_UNUSED( res )

  m_lineLengths.insert(m_blockArray.getCurrent(), count);
}

void HistoryScrollBlockArray::addLine(bool)
{
}

////////////////////////////////////////////////////////////////
// Compact History Scroll //////////////////////////////////////
////////////////////////////////////////////////////////////////
void* CompactHistoryBlock::allocate ( size_t length )
{
 Q_ASSERT ( length > 0 );
  if ( tail-blockStart+length > blockLength )
    return nullptr;

  void* block = tail;
  tail += length;
  //kDebug() << "allocated " << length << " bytes at address " << block;
  allocCount++;
  return block;
}

void CompactHistoryBlock::deallocate ( )
{
  allocCount--;
  Q_ASSERT ( allocCount >= 0 );
}

void* CompactHistoryBlockList::allocate(size_t size)
{
  CompactHistoryBlock* block;
  if ( list.isEmpty() || list.last()->remaining() < size)
  {
    block = new CompactHistoryBlock();
    list.append ( block );
    //kDebug() << "new block created, remaining " << block->remaining() << "number of blocks=" << list.size();
  }
  else
  {
    block = list.last();
    //kDebug() << "old block used, remaining " << block->remaining();
  }
  return block->allocate(size);
}

void CompactHistoryBlockList::deallocate(void* ptr)
{
  Q_ASSERT( !list.isEmpty());

  int i=0;
  CompactHistoryBlock *block = list.at(i);
  while ( i<list.size() && !block->contains(ptr) )
  {
    i++;
    block=list.at(i);
  }

  Q_ASSERT( i<list.size() );

  block->deallocate();

  if (!block->isInUse())
  {
    list.removeAt(i);
    delete block;
    //kDebug() << "block deleted, new size = " << list.size();
  }
}

CompactHistoryBlockList::~CompactHistoryBlockList()
{
  qDeleteAll ( list.begin(), list.end() );
  list.clear();
}

void* CompactHistoryLine::operator new (size_t size, CompactHistoryBlockList& blockList)
{
  return blockList.allocate(size);
}

CompactHistoryLine::CompactHistoryLine ( const TextLine& line, CompactHistoryBlockList& bList )
  : blockList(bList),
    formatLength(0)
{
  length=line.size();

  if (!line.empty()) {
    formatLength=1;
    int k=1;

    // count number of different formats in this text line
    Character c = line[0];
    while ( k<length )
    {
      if ( !(line[k].equalsFormat(c)))
      {
        formatLength++; // format change detected
        c=line[k];
      }
      k++;
    }

    //kDebug() << "number of different formats in string: " << formatLength;
    formatArray = (CharacterFormat*) blockList.allocate(sizeof(CharacterFormat)*formatLength);
    Q_ASSERT (formatArray!=nullptr);
    text = (quint16*) blockList.allocate(sizeof(quint16)*line.size());
    Q_ASSERT (text!=nullptr);

    length=line.size();
    wrapped=false;

    // record formats and their positions in the format array
    c=line[0];
    formatArray[0].setFormat ( c );
    formatArray[0].startPos=0;                        // there's always at least 1 format (for the entire line, unless a change happens)

    k=1;                                              // look for possible format changes
    int j=1;
    while ( k<length && j<formatLength )
    {
      if (!(line[k].equalsFormat(c)))
      {
        c=line[k];
        formatArray[j].setFormat(c);
        formatArray[j].startPos=k;
        //kDebug() << "format entry " << j << " at pos " << formatArray[j].startPos << " " << &(formatArray[j].startPos) ;
        j++;
      }
      k++;
    }

    // copy character values
    for ( int i=0; i<line.size(); i++ )
    {
      text[i]=line[i].character;
      //kDebug() << "char " << i << " at mem " << &(text[i]);
    }
  }
  //kDebug() << "line created, length " << length << " at " << &(length);
}

CompactHistoryLine::~CompactHistoryLine()
{
  //kDebug() << "~CHL";
  if (length>0) {
    blockList.deallocate(text);
    blockList.deallocate(formatArray);
  }
  blockList.deallocate(this);
}

void CompactHistoryLine::getCharacter ( int index, Character &r )
{
  Q_ASSERT ( index < length );
  int formatPos=0;
  while ( ( formatPos+1 ) < formatLength && index >= formatArray[formatPos+1].startPos )
    formatPos++;

  r.character=text[index];
  r.rendition = formatArray[formatPos].rendition;
  r.foregroundColor = formatArray[formatPos].fgColor;
  r.backgroundColor = formatArray[formatPos].bgColor;
}

void CompactHistoryLine::getCharacters ( Character* array, int length, int startColumn )
{
  Q_ASSERT ( startColumn >= 0 && length >= 0 );
  Q_ASSERT ( startColumn+length <= ( int ) getLength() );

  for ( int i=startColumn; i<length+startColumn; i++ )
  {
    getCharacter ( i, array[i-startColumn] );
  }
}


CompactHistoryScroll::CompactHistoryScroll ( unsigned int maxLineCount )
    : HistoryScroll ( new CompactHistoryType ( maxLineCount ) )
    ,lines()
    ,blockList()
{
  //kDebug() << "scroll of length " << maxLineCount << " created";
  setMaxNbLines ( maxLineCount );
}

CompactHistoryScroll::~CompactHistoryScroll()
{
  qDeleteAll ( lines.begin(), lines.end() );
  lines.clear();
}

void CompactHistoryScroll::addCellsVector ( const TextLine& cells )
{
  CompactHistoryLine *line;
  line = new(blockList) CompactHistoryLine ( cells, blockList );

  if ( lines.size() > ( int ) _maxLineCount )
  {
    delete lines.takeAt ( 0 );
  }
  lines.append ( line );
}

void CompactHistoryScroll::addCells ( const Character a[], int count )
{
  TextLine newLine ( count );
  std::copy ( a,a+count,newLine.begin() );
  addCellsVector ( newLine );
}

void CompactHistoryScroll::addLine ( bool previousWrapped )
{
  CompactHistoryLine *line = lines.last();
  //kDebug() << "last line at address " << line;
  line->setWrapped(previousWrapped);
}

int CompactHistoryScroll::getLines() const
{
  return lines.size();
}

int CompactHistoryScroll::getLineLen ( int lineNumber ) const
{
  Q_ASSERT ( lineNumber >= 0 && lineNumber < lines.size() );
  CompactHistoryLine* line = lines[lineNumber];
  //kDebug() << "request for line at address " << line;
  return line->getLength();
}


void CompactHistoryScroll::getCells ( int lineNumber, int startColumn, int count, Character buffer[] ) const
{
  if ( count == 0 ) return;
  Q_ASSERT ( lineNumber < lines.size() );
  CompactHistoryLine* line = lines[lineNumber];
  Q_ASSERT ( startColumn >= 0 );
  Q_ASSERT ( (unsigned int)startColumn <= line->getLength() - count );
  line->getCharacters ( buffer, count, startColumn );
}

void CompactHistoryScroll::setMaxNbLines ( unsigned int lineCount )
{
  _maxLineCount = lineCount;

  while (lines.size() > static_cast<int>(lineCount)) {
    delete lines.takeAt(0);
  }
  //kDebug() << "set max lines to: " << _maxLineCount;
}

bool CompactHistoryScroll::isWrappedLine ( int lineNumber ) const
{
  Q_ASSERT ( lineNumber < lines.size() );
  return lines[lineNumber]->isWrapped();
}

#endif

//////////////////////////////////////////////////////////////////////
// History Types
//////////////////////////////////////////////////////////////////////

HistoryType::HistoryType()
{
}

HistoryType::~HistoryType()
{
}

//////////////////////////////

HistoryTypeNone::HistoryTypeNone()
{
}

bool HistoryTypeNone::isEnabled() const
{
  return false;
}

HistoryScroll* HistoryTypeNone::scroll(HistoryScroll *old) const
{
  delete old;
  return new HistoryScrollNone();
}

int HistoryTypeNone::maximumLineCount() const
{
  return 0;
}

//////////////////////////////

#ifndef Q_OS_WIN

HistoryTypeBlockArray::HistoryTypeBlockArray(size_t size)
  : m_size(size)
{
}

bool HistoryTypeBlockArray::isEnabled() const
{
  return true;
}

int HistoryTypeBlockArray::maximumLineCount() const
{
  return m_size;
}

HistoryScroll* HistoryTypeBlockArray::scroll(HistoryScroll *old) const
{
  delete old;
  return new HistoryScrollBlockArray(m_size);
}

#endif

//////////////////////////////

HistoryTypeBuffer::HistoryTypeBuffer(unsigned int nbLines)
  : m_nbLines(nbLines)
{
}

bool HistoryTypeBuffer::isEnabled() const
{
  return true;
}

int HistoryTypeBuffer::maximumLineCount() const
{
  return m_nbLines;
}

HistoryScroll* HistoryTypeBuffer::scroll(HistoryScroll *old) const
{
  if (old)
  {
    HistoryScrollBuffer *oldBuffer = dynamic_cast<HistoryScrollBuffer*>(old);
    if (oldBuffer)
    {
       oldBuffer->setMaxNbLines(m_nbLines);
       return oldBuffer;
    }

    HistoryScroll *newScroll = new HistoryScrollBuffer(m_nbLines);
    int lines = old->getLines();
    int startLine = 0;
    if (lines > static_cast<int>(m_nbLines))
       startLine = lines - m_nbLines;

    Character line[LINE_SIZE];
    for(int i = startLine; i < lines; i++)
    {
       int size = old->getLineLen(i);
       if (size > LINE_SIZE)
       {
          Character *tmp_line = new Character[size];
          old->getCells(i, 0, size, tmp_line);
          newScroll->addCells(tmp_line, size);
          newScroll->addLine(old->isWrappedLine(i));
          delete [] tmp_line;
       }
       else
       {
          old->getCells(i, 0, size, line);
          newScroll->addCells(line, size);
          newScroll->addLine(old->isWrappedLine(i));
       }
    }
    delete old;
    return newScroll;
  }
  return new HistoryScrollBuffer(m_nbLines);
}

//////////////////////////////

HistoryTypeFile::HistoryTypeFile(const QString& fileName)
  : m_fileName(fileName)
{
}

bool HistoryTypeFile::isEnabled() const
{
  return true;
}

const QString& HistoryTypeFile::getFileName() const
{
  return m_fileName;
}

HistoryScroll* HistoryTypeFile::scroll(HistoryScroll *old) const
{
  if (dynamic_cast<HistoryFile *>(old))
     return old; // Unchanged.

  HistoryScroll *newScroll = new HistoryScrollFile(m_fileName);

  Character line[LINE_SIZE];
  int lines = (old != nullptr) ? old->getLines() : 0;
  for(int i = 0; i < lines; i++)
  {
     int size = old->getLineLen(i);
     if (size > LINE_SIZE)
     {
        Character *tmp_line = new Character[size];
        old->getCells(i, 0, size, tmp_line);
        newScroll->addCells(tmp_line, size);
        newScroll->addLine(old->isWrappedLine(i));
        delete [] tmp_line;
     }
     else
     {
        old->getCells(i, 0, size, line);
        newScroll->addCells(line, size);
        newScroll->addLine(old->isWrappedLine(i));
     }
  }

  delete old;
  return newScroll;
}

int HistoryTypeFile::maximumLineCount() const
{
  return 0;
}

//////////////////////////////

#ifndef Q_OS_WIN


CompactHistoryType::CompactHistoryType ( unsigned int nbLines )
    : m_nbLines ( nbLines )
{
}

bool CompactHistoryType::isEnabled() const
{
  return true;
}

int CompactHistoryType::maximumLineCount() const
{
  return m_nbLines;
}

HistoryScroll* CompactHistoryType::scroll ( HistoryScroll *old ) const
{
  if ( old )
  {
    CompactHistoryScroll *oldBuffer = dynamic_cast<CompactHistoryScroll*> ( old );
    if ( oldBuffer )
    {
      oldBuffer->setMaxNbLines ( m_nbLines );
      return oldBuffer;
    }
    delete old;
  }
  return new CompactHistoryScroll ( m_nbLines );
}

#endif