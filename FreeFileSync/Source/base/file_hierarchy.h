// *****************************************************************************
// * This file is part of the FreeFileSync project. It is distributed under    *
// * GNU General Public License: https://www.gnu.org/licenses/gpl-3.0          *
// * Copyright (C) Zenju (zenju AT freefilesync DOT org) - All Rights Reserved *
// *****************************************************************************

#ifndef FILE_HIERARCHY_H_257235289645296
#define FILE_HIERARCHY_H_257235289645296

#include <string>
#include <memory>
#include <list>
#include <functional>
#include <unordered_set>
#include <unordered_map>
#include "structures.h"
#include "path_filter.h"
#include "../afs/abstract.h"


namespace fff
{
struct FileAttributes
{
    FileAttributes() {}
    FileAttributes(time_t modTimeIn,
                   uint64_t fileSizeIn,
                   AFS::FingerPrint filePrintIn,
                   bool followedSymlink) :
        modTime(modTimeIn),
        fileSize(fileSizeIn),
        filePrint(filePrintIn),
        isFollowedSymlink(followedSymlink)
    {
        static_assert(std::is_signed_v<time_t>, "... and signed!");
    }

    time_t modTime = 0; //number of seconds since Jan. 1st 1970 GMT
    uint64_t fileSize = 0;
    AFS::FingerPrint filePrint = 0; //optional
    bool isFollowedSymlink = false;

    std::strong_ordering operator<=>(const FileAttributes&) const = default;
};


struct LinkAttributes
{
    LinkAttributes() {}
    explicit LinkAttributes(time_t modTimeIn) : modTime(modTimeIn) {}

    time_t modTime = 0; //number of seconds since Jan. 1st 1970 GMT
};


struct FolderAttributes
{
    FolderAttributes() {}
    explicit FolderAttributes(bool isSymlink) :
        isFollowedSymlink(isSymlink) {}

    bool isFollowedSymlink = false;
};


enum class SelectSide
{
    left,
    right
};


template <SelectSide side>
constexpr SelectSide getOtherSide = side == SelectSide::left ? SelectSide::right : SelectSide::left;


template <SelectSide side, class T> inline
T& selectParam(T& left, T& right)
{
    if constexpr (side == SelectSide::left)
        return left;
    else
        return right;
}

//------------------------------------------------------------------

std::wstring getShortDisplayNameForFolderPair(const AbstractPath& itemPathL, const AbstractPath& itemPathR);

//------------------------------------------------------------------

struct FolderContainer
{
    //------------------------------------------------------------------
    //key: raw file name, without any (Unicode) normalization, preserving original upper-/lower-case
    //"Changing data [...] to NFC would cause interoperability problems. Always leave data as it is."
    using FolderList  = std::unordered_map<Zstring, std::pair<FolderAttributes, FolderContainer>>;
    using FileList    = std::unordered_map<Zstring, FileAttributes>;
    using SymlinkList = std::unordered_map<Zstring, LinkAttributes>;
    //------------------------------------------------------------------

    FolderContainer() = default;
    FolderContainer           (const FolderContainer&) = delete; //catch accidental (and unnecessary) copying
    FolderContainer& operator=(const FolderContainer&) = delete; //

    FileList    files;
    SymlinkList symlinks; //non-followed symlinks
    FolderList  folders;

    void addFile(const Zstring& itemName, const FileAttributes& attr)
    {
        const auto [it, inserted] = files.emplace(itemName, attr);
        if (!inserted) //update entry if already existing (e.g. during folder traverser "retry")
            it->second = attr;
    }

    void addLink(const Zstring& itemName, const LinkAttributes& attr)
    {
        const auto [it, inserted] = symlinks.emplace(itemName, attr);
        if (!inserted)
            it->second = attr;
    }

    FolderContainer& addFolder(const Zstring& itemName, const FolderAttributes& attr)
    {
        auto& p = folders[itemName]; //value default-construction is okay here
        p.first = attr;
        return p.second;

        //const auto [it, inserted] = folders.emplace(itemName, std::pair<FolderAttributes, FolderContainer>(attr, FolderContainer()));
        //if (!inserted)
        //  it->second.first = attr;
        //return it->second.second;
    }
};

class BaseFolderPair;
class FolderPair;
class FilePair;
class SymlinkPair;
class FileSystemObject;

/*------------------------------------------------------------------
    inheritance diagram:

         ObjectMgr        PathInformation
            /|\                 /|\
             |________  _________|_________
                      ||                   |
               FileSystemObject     ContainerObject
                     /|\                  /|\
           ___________|___________   ______|______
          |           |           | |             |
     SymlinkPair   FilePair    FolderPair   BaseFolderPair

------------------------------------------------------------------*/

struct PathInformation //diamond-shaped inheritance!
{
    virtual ~PathInformation() {}

    template <SelectSide side> AbstractPath getAbstractPath() const;
    template <SelectSide side> Zstring      getRelativePath() const; //get path relative to base sync dir (without leading/trailing FILE_NAME_SEPARATOR)
    Zstring getRelativePathAny() const { return getRelativePathL(); } //side doesn't matter

private:
    virtual AbstractPath getAbstractPathL() const = 0; //implemented by FileSystemObject + BaseFolderPair
    virtual AbstractPath getAbstractPathR() const = 0; //

    virtual Zstring getRelativePathL() const = 0; //implemented by SymlinkPair/FilePair + ContainerObject
    virtual Zstring getRelativePathR() const = 0; //
};

template <> inline AbstractPath PathInformation::getAbstractPath<SelectSide::left >() const { return getAbstractPathL(); }
template <> inline AbstractPath PathInformation::getAbstractPath<SelectSide::right>() const { return getAbstractPathR(); }

template <> inline Zstring PathInformation::getRelativePath<SelectSide::left >() const { return getRelativePathL(); }
template <> inline Zstring PathInformation::getRelativePath<SelectSide::right>() const { return getRelativePathR(); }

//------------------------------------------------------------------

class ContainerObject : public virtual PathInformation
{
    friend class FolderPair;
    friend class FileSystemObject;

public:
    using FileList    = std::list<FilePair>;    //MergeSides::execute() requires a structure that doesn't invalidate pointers after push_back()
    using SymlinkList = std::list<SymlinkPair>; //
    using FolderList  = std::list<FolderPair>;

    FolderPair& addFolder(const Zstring&          itemNameL,
                          const FolderAttributes& left,    //file exists on both sides
                          CompareDirResult        defaultCmpResult,
                          const Zstring&          itemNameR,
                          const FolderAttributes& right);

    template <SelectSide side>
    FolderPair& addFolder(const Zstring& itemName, //dir exists on one side only
                          const FolderAttributes& attr);

    FilePair& addFile(const Zstring&        itemNameL,
                      const FileAttributes& left,          //file exists on both sides
                      CompareFileResult    defaultCmpResult,
                      const Zstring&        itemNameR,
                      const FileAttributes& right);

    template <SelectSide side>
    FilePair& addFile(const Zstring&        itemName, //file exists on one side only
                      const FileAttributes& attr);

    SymlinkPair& addLink(const Zstring&        itemNameL,
                         const LinkAttributes& left, //link exists on both sides
                         CompareSymlinkResult  defaultCmpResult,
                         const Zstring&        itemNameR,
                         const LinkAttributes& right);

    template <SelectSide side>
    SymlinkPair& addLink(const Zstring&        itemName, //link exists on one side only
                         const LinkAttributes& attr);

    const FileList& refSubFiles() const { return subFiles_; }
    /**/  FileList& refSubFiles()       { return subFiles_; }

    const SymlinkList& refSubLinks() const { return subLinks_; }
    /**/  SymlinkList& refSubLinks()       { return subLinks_; }

    const FolderList& refSubFolders() const { return subFolders_; }
    /**/  FolderList& refSubFolders()       { return subFolders_; }

    const BaseFolderPair& getBase() const { return base_; }
    /**/  BaseFolderPair& getBase()       { return base_; }

protected:
    ContainerObject(BaseFolderPair& baseFolder) : //used during BaseFolderPair constructor
        base_(baseFolder) {} //take reference only: baseFolder *not yet* fully constructed at this point!

    ContainerObject(const FileSystemObject& fsAlias); //used during FolderPair constructor

    virtual ~ContainerObject() {} //don't need polymorphic deletion, but we have a vtable anyway

    virtual void flip();

    void removeEmptyRec();

    template <SelectSide side>
    void updateRelPathsRecursion(const FileSystemObject& fsAlias);

private:
    ContainerObject           (const ContainerObject&) = delete; //this class is referenced by its child elements => make it non-copyable/movable!
    ContainerObject& operator=(const ContainerObject&) = delete;

    virtual void notifySyncCfgChanged() {}

    Zstring getRelativePathL() const override { return relPathL_; }
    Zstring getRelativePathR() const override { return relPathR_; }

    FileList    subFiles_;
    SymlinkList subLinks_;
    FolderList  subFolders_;

    Zstring relPathL_; //path relative to base sync dir (without leading/trailing FILE_NAME_SEPARATOR)
    Zstring relPathR_; //

    BaseFolderPair& base_;
};

//------------------------------------------------------------------

enum class BaseFolderStatus
{
    existing,
    notExisting,
    failure,
};

class BaseFolderPair : public ContainerObject //synchronization base directory
{
public:
    BaseFolderPair(const AbstractPath& folderPathLeft,
                   BaseFolderStatus folderStatusLeft,
                   const AbstractPath& folderPathRight,
                   BaseFolderStatus folderStatusRight,
                   const FilterRef& filter,
                   CompareVariant cmpVar,
                   int fileTimeTolerance,
                   const std::vector<unsigned int>& ignoreTimeShiftMinutes) :
        ContainerObject(*this), //trust that ContainerObject knows that *this is not yet fully constructed!
        filter_(filter), cmpVar_(cmpVar), fileTimeTolerance_(fileTimeTolerance), ignoreTimeShiftMinutes_(ignoreTimeShiftMinutes),
        folderStatusLeft_ (folderStatusLeft),
        folderStatusRight_(folderStatusRight),
        folderPathLeft_(folderPathLeft),
        folderPathRight_(folderPathRight) {}

    static void removeEmpty(BaseFolderPair& baseFolder) { baseFolder.removeEmptyRec(); } //physically remove all invalid entries (where both sides are empty) recursively

    template <SelectSide side> BaseFolderStatus getFolderStatus() const; //base folder status at the time of comparison!
    template <SelectSide side> void setFolderStatus(BaseFolderStatus value); //update after creating the directory in FFS

    //get settings which were used while creating BaseFolderPair:
    const PathFilter&   getFilter() const { return filter_.ref(); }
    CompareVariant getCompVariant() const { return cmpVar_; }
    int      getFileTimeTolerance() const { return fileTimeTolerance_; }
    const std::vector<unsigned int>& getIgnoredTimeShift() const { return ignoreTimeShiftMinutes_; }

    void flip() override;

private:
    AbstractPath getAbstractPathL() const override { return folderPathLeft_; }
    AbstractPath getAbstractPathR() const override { return folderPathRight_; }

    const FilterRef filter_; //filter used while scanning directory: represents sub-view of actual files!
    const CompareVariant cmpVar_;
    const int fileTimeTolerance_;
    const std::vector<unsigned int> ignoreTimeShiftMinutes_;

    BaseFolderStatus folderStatusLeft_;
    BaseFolderStatus folderStatusRight_;

    AbstractPath folderPathLeft_;
    AbstractPath folderPathRight_;
};


//get rid of shared_ptr indirection
template <class IterImpl, //underlying iterator type
          class T>        //target value type
class DerefIter
{
public:
    using iterator_category = std::bidirectional_iterator_tag;
    using value_type = T;
    using difference_type = ptrdiff_t;
    using pointer   = T*;
    using reference = T&;

    DerefIter() {}
    DerefIter(IterImpl it) : it_(it) {}
    //DerefIter(const DerefIter& other) : it_(other.it_) {}
    DerefIter& operator++() { ++it_; return *this; }
    DerefIter& operator--() { --it_; return *this; }
    inline friend DerefIter operator++(DerefIter& it, int) { return it++; }
    inline friend DerefIter operator--(DerefIter& it, int) { return it--; }
    inline friend ptrdiff_t operator-(const DerefIter& lhs, const DerefIter& rhs) { return lhs.it_ - rhs.it_; }
    bool operator==(const DerefIter&) const = default;
    T& operator* () const { return  **it_; }
    T* operator->() const { return &** it_; }
private:
    IterImpl it_{};
};


using FolderComparison = std::vector<std::shared_ptr<BaseFolderPair>>; //make sure pointers to sub-elements remain valid
//don't change this back to std::vector<BaseFolderPair> too easily: comparison uses push_back to add entries which may result in a full copy!

DerefIter<typename FolderComparison::iterator,             BaseFolderPair> inline begin(      FolderComparison& vect) { return vect.begin(); }
DerefIter<typename FolderComparison::iterator,             BaseFolderPair> inline end  (      FolderComparison& vect) { return vect.end  (); }
DerefIter<typename FolderComparison::const_iterator, const BaseFolderPair> inline begin(const FolderComparison& vect) { return vect.begin(); }
DerefIter<typename FolderComparison::const_iterator, const BaseFolderPair> inline end  (const FolderComparison& vect) { return vect.end  (); }

//------------------------------------------------------------------
struct FSObjectVisitor
{
    virtual ~FSObjectVisitor() {}
    virtual void visit(const FilePair&    file   ) = 0;
    virtual void visit(const SymlinkPair& symlink) = 0;
    virtual void visit(const FolderPair&  folder ) = 0;
};


//inherit from this class to allow safe random access by id instead of unsafe raw pointer
//allow for similar semantics like std::weak_ptr without having to use std::shared_ptr
template <class T>
class ObjectMgr
{
public:
    using ObjectId      =       ObjectMgr*;
    using ObjectIdConst = const ObjectMgr*;

    ObjectIdConst  getId() const { return this; }
    /**/  ObjectId getId()       { return this; }

    static const T* retrieve(ObjectIdConst id) //returns nullptr if object is not valid anymore
    {
        return static_cast<const T*>(activeObjects_.contains(id) ? id : nullptr);
    }
    static T* retrieve(ObjectId id) { return const_cast<T*>(retrieve(static_cast<ObjectIdConst>(id))); }

protected:
    ObjectMgr () { activeObjects_.insert(this); }
    ~ObjectMgr() { activeObjects_.erase (this); }

private:
    ObjectMgr           (const ObjectMgr& rhs) = delete;
    ObjectMgr& operator=(const ObjectMgr& rhs) = delete; //it's not well-defined what copying an objects means regarding object-identity in this context

    //our global ObjectMgr is not thread-safe (and currently does not need to be!)
    //assert(runningOnMainThread()); -> still, may be accessed by synchronization worker threads, one thread at a time
    static inline std::unordered_set<const ObjectMgr*> activeObjects_; //external linkage!
};

//------------------------------------------------------------------

class FileSystemObject : public ObjectMgr<FileSystemObject>, public virtual PathInformation
{
public:
    virtual void accept(FSObjectVisitor& visitor) const = 0;

    bool isPairEmpty() const; //true, if both sides are empty
    template <SelectSide side> bool isEmpty() const;

    //path getters always return valid values, even if isEmpty<side>()!
    Zstring getItemNameAny() const; //like getItemName() but without bias to which side is returned
    template <SelectSide side> Zstring getItemName() const; //case sensitive!

    //comparison result
    CompareFileResult getCategory() const { return cmpResult_; }
    Zstringc getCatExtraDescription() const; //only filled if getCategory() == FILE_CONFLICT or FILE_DIFFERENT_METADATA

    //sync settings
    SyncDirection getSyncDir() const { return syncDir_; }
    void setSyncDir(SyncDirection newDir);
    void setSyncDirConflict(const Zstringc& description); //set syncDir = SyncDirection::none + fill conflict description

    bool isActive() const { return selectedForSync_; }
    void setActive(bool active);

    //sync operation
    virtual SyncOperation testSyncOperation(SyncDirection testSyncDir) const; //semantics: "what if"! assumes "active, no conflict, no recursion (directory)!
    virtual SyncOperation getSyncOperation() const;
    std::wstring getSyncOpConflict() const; //return conflict when determining sync direction or (still unresolved) conflict during categorization

    template <SelectSide side> void removeObject(); //removes file or directory (recursively!) without physically removing the element: used by manual deletion

    const ContainerObject& parent() const { return parent_; }
    /**/  ContainerObject& parent()       { return parent_; }
    const BaseFolderPair& base() const  { return parent_.getBase(); }
    /**/  BaseFolderPair& base()        { return parent_.getBase(); }

    //for use during init in "CompareProcess" only:
    template <CompareFileResult res> void setCategory();
    void setCategoryConflict    (const Zstringc& description);
    void setCategoryDiffMetadata(const Zstringc& description);

protected:
    FileSystemObject(const Zstring& itemNameL,
                     const Zstring& itemNameR,
                     ContainerObject& parentObj,
                     CompareFileResult defaultCmpResult) :
        cmpResult_(defaultCmpResult),
        itemNameL_(itemNameL),
        itemNameR_(itemNameL == itemNameR ? itemNameL : itemNameR), //perf: no measurable speed drawback; -3% peak memory => further needed by ContainerObject construction!
        parent_(parentObj)
    {
        assert(itemNameL_.c_str() == itemNameR_.c_str() || itemNameL_ != itemNameR_); //also checks ref-counted string precondition
        parent_.notifySyncCfgChanged();
    }

    virtual ~FileSystemObject() {} //don't need polymorphic deletion, but we have a vtable anyway
    //must not call parent here, it is already partially destroyed and nothing more than a pure ContainerObject!

    virtual void flip();
    virtual void notifySyncCfgChanged() { parent().notifySyncCfgChanged(); /*propagate!*/ }

    void setSynced(const Zstring& itemName);

private:
    FileSystemObject           (const FileSystemObject&) = delete;
    FileSystemObject& operator=(const FileSystemObject&) = delete;

    AbstractPath getAbstractPathL() const override { return AFS::appendRelPath(base().getAbstractPath<SelectSide::left >(), getRelativePath<SelectSide::left >()); }
    AbstractPath getAbstractPathR() const override { return AFS::appendRelPath(base().getAbstractPath<SelectSide::right>(), getRelativePath<SelectSide::right>()); }

    virtual void removeObjectL() = 0;
    virtual void removeObjectR() = 0;

    template <SelectSide side>
    void propagateChangedItemName(const Zstring& itemNameOld); //required after any itemName changes

    //categorization
    Zstringc cmpResultDescr_; //only filled if getCategory() == FILE_CONFLICT or FILE_DIFFERENT_METADATA
    //conserve memory (avoid std::string SSO overhead + allow ref-counting!)
    CompareFileResult cmpResult_; //although this uses 4 bytes there is currently *no* space wasted in class layout!

    bool selectedForSync_ = true;

    //Note: we model *four* states with following two variables => "syncDirectionConflict is empty or syncDir == NONE" is a class invariant!!!
    SyncDirection syncDir_ = SyncDirection::none; //1 byte: optimize memory layout!
    Zstringc syncDirectionConflict_; //non-empty if we have a conflict setting sync-direction
    //conserve memory (avoid std::string SSO overhead + allow ref-counting!)

    Zstring itemNameL_; //slightly redundant under Linux, but on Windows the "same" file paths can differ in case
    Zstring itemNameR_; //use as indicator: an empty name means: not existing on this side!

    ContainerObject& parent_;
};

//------------------------------------------------------------------


class FolderPair : public FileSystemObject, public ContainerObject
{
    friend class ContainerObject;

public:
    void accept(FSObjectVisitor& visitor) const override;

    CompareDirResult getDirCategory() const; //returns actually used subset of CompareFileResult

    FolderPair(const Zstring& itemNameL, //use empty itemName if "not existing"
               const FolderAttributes& attrL,
               CompareDirResult defaultCmpResult,
               const Zstring& itemNameR,
               const FolderAttributes& attrR,
               ContainerObject& parentObj) :
        FileSystemObject(itemNameL, itemNameR, parentObj, static_cast<CompareFileResult>(defaultCmpResult)),
        ContainerObject(static_cast<FileSystemObject&>(*this)), //FileSystemObject fully constructed at this point!
        attrL_(attrL),
        attrR_(attrR) {}

    template <SelectSide side> bool isFollowedSymlink() const;

    SyncOperation getSyncOperation() const override;

    template <SelectSide sideTrg>
    void setSyncedTo(const Zstring& itemName, bool isSymlinkTrg, bool isSymlinkSrc); //call after sync, sets DIR_EQUAL

private:
    void flip         () override;
    void removeObjectL() override;
    void removeObjectR() override;
    void notifySyncCfgChanged() override { syncOpBuffered_ = {}; FileSystemObject::notifySyncCfgChanged(); ContainerObject::notifySyncCfgChanged(); }

    mutable std::optional<SyncOperation> syncOpBuffered_; //determining sync-op for directory may be expensive as it depends on child-objects => buffer

    FolderAttributes attrL_;
    FolderAttributes attrR_;
};


//------------------------------------------------------------------

class FilePair : public FileSystemObject
{
    friend class ContainerObject; //construction

public:
    void accept(FSObjectVisitor& visitor) const override;

    FilePair(const Zstring&        itemNameL, //use empty string if "not existing"
             const FileAttributes& attrL,
             CompareFileResult     defaultCmpResult,
             const Zstring&        itemNameR, //
             const FileAttributes& attrR,
             ContainerObject& parentObj) :
        FileSystemObject(itemNameL, itemNameR, parentObj, defaultCmpResult),
        attrL_(attrL),
        attrR_(attrR) {}

    template <SelectSide side> time_t       getLastWriteTime() const;
    template <SelectSide side> uint64_t          getFileSize() const;
    template <SelectSide side> bool        isFollowedSymlink() const;
    template <SelectSide side> FileAttributes  getAttributes() const;
    template <SelectSide side> AFS::FingerPrint getFilePrint() const;
    template <SelectSide side> void clearFilePrint();

    void setMoveRef(ObjectId refId) { moveFileRef_ = refId; } //reference to corresponding renamed file
    ObjectId getMoveRef() const { return moveFileRef_; } //may be nullptr

    CompareFileResult getFileCategory() const;

    SyncOperation testSyncOperation(SyncDirection testSyncDir) const override; //semantics: "what if"! assumes "active, no conflict, no recursion (directory)!
    SyncOperation getSyncOperation() const override;

    template <SelectSide sideTrg>
    void setSyncedTo(const Zstring& itemName, //call after sync, sets FILE_EQUAL
                     uint64_t fileSize,
                     int64_t lastWriteTimeTrg,
                     int64_t lastWriteTimeSrc,
                     AFS::FingerPrint filePrintTrg,
                     AFS::FingerPrint filePrintSrc,
                     bool isSymlinkTrg,
                     bool isSymlinkSrc);

private:
    Zstring getRelativePathL() const override { return appendPath(parent().getRelativePath<SelectSide::left >(), getItemName<SelectSide::left >()); }
    Zstring getRelativePathR() const override { return appendPath(parent().getRelativePath<SelectSide::right>(), getItemName<SelectSide::right>()); }

    SyncOperation applyMoveOptimization(SyncOperation op) const;

    void flip         () override;
    void removeObjectL() override { attrL_ = FileAttributes(); }
    void removeObjectR() override { attrR_ = FileAttributes(); }

    FileAttributes attrL_;
    FileAttributes attrR_;

    ObjectId moveFileRef_ = nullptr; //optional, filled by redetermineSyncDirection()
};

//------------------------------------------------------------------

class SymlinkPair : public FileSystemObject //this class models a TRUE symbolic link, i.e. one that is NEVER dereferenced: deref-links should be directly placed in class File/FolderPair
{
    friend class ContainerObject; //construction

public:
    void accept(FSObjectVisitor& visitor) const override;

    template <SelectSide side> time_t getLastWriteTime() const; //write time of the link, NOT target!

    CompareSymlinkResult getLinkCategory()   const; //returns actually used subset of CompareFileResult

    SymlinkPair(const Zstring&         itemNameL, //use empty string if "not existing"
                const LinkAttributes&  attrL,
                CompareSymlinkResult   defaultCmpResult,
                const Zstring&         itemNameR, //use empty string if "not existing"
                const LinkAttributes&  attrR,
                ContainerObject& parentObj) :
        FileSystemObject(itemNameL, itemNameR, parentObj, static_cast<CompareFileResult>(defaultCmpResult)),
        attrL_(attrL),
        attrR_(attrR) {}

    template <SelectSide sideTrg>
    void setSyncedTo(const Zstring& itemName, //call after sync, sets SYMLINK_EQUAL
                     int64_t lastWriteTimeTrg,
                     int64_t lastWriteTimeSrc);

private:
    Zstring getRelativePathL() const override { return appendPath(parent().getRelativePath<SelectSide::left >(), getItemName<SelectSide::left >()); }
    Zstring getRelativePathR() const override { return appendPath(parent().getRelativePath<SelectSide::right>(), getItemName<SelectSide::right>()); }

    void flip()          override;
    void removeObjectL() override { attrL_ = LinkAttributes(); }
    void removeObjectR() override { attrR_ = LinkAttributes(); }

    LinkAttributes attrL_;
    LinkAttributes attrR_;
};

//------------------------------------------------------------------

//generic type descriptions (usecase CSV legend, sync config)
std::wstring getCategoryDescription(CompareFileResult cmpRes);
std::wstring getSyncOpDescription  (SyncOperation op);

//item-specific type descriptions
std::wstring getCategoryDescription(const FileSystemObject& fsObj);
std::wstring getSyncOpDescription  (const FileSystemObject& fsObj);

//------------------------------------------------------------------

namespace impl
{
template <class Function1, class Function2, class Function3>
struct FSObjectLambdaVisitor : public FSObjectVisitor
{
    FSObjectLambdaVisitor(Function1 onFolder,
                          Function2 onFile,
                          Function3 onSymlink) : //unifying assignment
        onFolder_(std::move(onFolder)), onFile_(std::move(onFile)), onSymlink_(std::move(onSymlink)) {}
private:
    void visit(const FolderPair&  folder ) override { onFolder_ (folder);  }
    void visit(const FilePair&    file   ) override { onFile_   (file);    }
    void visit(const SymlinkPair& symlink) override { onSymlink_(symlink); }

    const Function1 onFolder_;
    const Function2 onFile_;
    const Function3 onSymlink_;
};
}

template <class Function1, class Function2, class Function3> inline
void visitFSObject(const FileSystemObject& fsObj, Function1 onFolder, Function2 onFile, Function3 onSymlink)
{
    impl::FSObjectLambdaVisitor<Function1, Function2, Function3> visitor(onFolder, onFile, onSymlink);
    fsObj.accept(visitor);
}

//------------------------------------------------------------------

namespace impl
{
template <class Function1, class Function2, class Function3>
class RecursiveObjectVisitor
{
public:
    RecursiveObjectVisitor(Function1 onFolder,
                           Function2 onFile,
                           Function3 onSymlink) : //unifying assignment
        onFolder_(std::move(onFolder)), onFile_(std::move(onFile)), onSymlink_(std::move(onSymlink)) {}

    void execute(ContainerObject& hierObj)
    {
        for (FilePair& file : hierObj.refSubFiles())
            onFile_(file);
        for (SymlinkPair& symlink : hierObj.refSubLinks())
            onSymlink_(symlink);
        for (FolderPair& subFolder : hierObj.refSubFolders())
        {
            onFolder_(subFolder);
            execute(subFolder);
        }
    }

private:
    RecursiveObjectVisitor           (const RecursiveObjectVisitor&) = delete;
    RecursiveObjectVisitor& operator=(const RecursiveObjectVisitor&) = delete;

    const Function1 onFolder_;
    const Function2 onFile_;
    const Function3 onSymlink_;
};
}

template <class Function1, class Function2, class Function3> inline
void visitFSObjectRecursively(ContainerObject& hierObj, //consider contained items only
                              Function1 onFolder,
                              Function2 onFile,
                              Function3 onSymlink)
{
    impl::RecursiveObjectVisitor(onFolder, onFile, onSymlink).execute(hierObj);
}

template <class Function1, class Function2, class Function3> inline
void visitFSObjectRecursively(FileSystemObject& fsObj, //consider item and contained items (if folder)
                              Function1 onFolder,
                              Function2 onFile,
                              Function3 onSymlink)
{
    visitFSObject(fsObj, [onFolder, onFile, onSymlink](const FolderPair& folder)
    {
        onFolder(const_cast<FolderPair&>(folder));
        impl::RecursiveObjectVisitor(onFolder, onFile, onSymlink).execute(const_cast<FolderPair&>(folder));
    },
    [onFile   ](const FilePair&       file) { onFile   (const_cast<FilePair&   >(file   )); },  //physical object is not const anyway
    [onSymlink](const SymlinkPair& symlink) { onSymlink(const_cast<SymlinkPair&>(symlink)); }); //
}


















//--------------------- implementation ------------------------------------------

//inline virtual... admittedly its use may be limited
inline void FilePair   ::accept(FSObjectVisitor& visitor) const { visitor.visit(*this); }
inline void FolderPair ::accept(FSObjectVisitor& visitor) const { visitor.visit(*this); }
inline void SymlinkPair::accept(FSObjectVisitor& visitor) const { visitor.visit(*this); }


inline
CompareFileResult FilePair::getFileCategory() const
{
    return getCategory();
}


inline
CompareDirResult FolderPair::getDirCategory() const
{
    return static_cast<CompareDirResult>(getCategory());
}


inline
Zstringc FileSystemObject::getCatExtraDescription() const
{
    assert(getCategory() == FILE_CONFLICT || getCategory() == FILE_DIFFERENT_METADATA);
    return cmpResultDescr_;
}


inline
void FileSystemObject::setSyncDir(SyncDirection newDir)
{
    syncDir_ = newDir;
    syncDirectionConflict_.clear();

    notifySyncCfgChanged();
}


inline
void FileSystemObject::setSyncDirConflict(const Zstringc& description)
{
    assert(!description.empty());
    syncDir_ = SyncDirection::none;
    syncDirectionConflict_ = description;

    notifySyncCfgChanged();
}


inline
std::wstring FileSystemObject::getSyncOpConflict() const
{
    assert(getSyncOperation() == SO_UNRESOLVED_CONFLICT);
    return zen::utfTo<std::wstring>(syncDirectionConflict_);
}


inline
void FileSystemObject::setActive(bool active)
{
    selectedForSync_ = active;
    notifySyncCfgChanged();
}


template <SelectSide side> inline
bool FileSystemObject::isEmpty() const
{
    return selectParam<side>(itemNameL_, itemNameR_).empty();
}


inline
bool FileSystemObject::isPairEmpty() const
{
    return isEmpty<SelectSide::left>() && isEmpty<SelectSide::right>();
}


template <SelectSide side> inline
Zstring FileSystemObject::getItemName() const
{
    //assert(!itemNameL_.empty() || !itemNameR_.empty()); //-> file pair might be temporarily empty (until permanently removed after sync)

    const Zstring& itemName = selectParam<side>(itemNameL_, itemNameR_); //empty if not existing
    if (!itemName.empty()) //avoid ternary-WTF! (implicit copy-constructor call!!!!!!)
        return itemName;
    return selectParam<getOtherSide<side>>(itemNameL_, itemNameR_);
}


inline
Zstring FileSystemObject::getItemNameAny() const
{
    return getItemName<SelectSide::left>(); //side doesn't matter
}


template <> inline
void FileSystemObject::removeObject<SelectSide::left>()
{
    const Zstring itemNameOld = getItemName<SelectSide::left>();

    cmpResult_ = isEmpty<SelectSide::right>() ? FILE_EQUAL : FILE_RIGHT_SIDE_ONLY;
    itemNameL_.clear();
    removeObjectL();

    setSyncDir(SyncDirection::none); //calls notifySyncCfgChanged()
    propagateChangedItemName<SelectSide::left>(itemNameOld);
}


template <> inline
void FileSystemObject::removeObject<SelectSide::right>()
{
    const Zstring itemNameOld = getItemName<SelectSide::right>();

    cmpResult_ = isEmpty<SelectSide::left>() ? FILE_EQUAL : FILE_LEFT_SIDE_ONLY;
    itemNameR_.clear();
    removeObjectR();

    setSyncDir(SyncDirection::none); //calls notifySyncCfgChanged()
    propagateChangedItemName<SelectSide::right>(itemNameOld);
}


inline
void FileSystemObject::setSynced(const Zstring& itemName)
{
    const Zstring itemNameOldL = getItemName<SelectSide::left>();
    const Zstring itemNameOldR = getItemName<SelectSide::right>();

    assert(!isPairEmpty());
    itemNameR_ = itemNameL_ = itemName;
    cmpResult_ = FILE_EQUAL;
    setSyncDir(SyncDirection::none);

    propagateChangedItemName<SelectSide::left >(itemNameOldL);
    propagateChangedItemName<SelectSide::right>(itemNameOldR);
}


template <CompareFileResult res> inline
void FileSystemObject::setCategory()
{
    cmpResult_ = res;
}
template <> void FileSystemObject::setCategory<FILE_CONFLICT>          () = delete; //
template <> void FileSystemObject::setCategory<FILE_DIFFERENT_METADATA>() = delete; //deny use
template <> void FileSystemObject::setCategory<FILE_LEFT_SIDE_ONLY>    () = delete; //
template <> void FileSystemObject::setCategory<FILE_RIGHT_SIDE_ONLY>   () = delete; //

inline
void FileSystemObject::setCategoryConflict(const Zstringc& description)
{
    assert(!description.empty());
    cmpResult_ = FILE_CONFLICT;
    cmpResultDescr_ = description;
}

inline
void FileSystemObject::setCategoryDiffMetadata(const Zstringc& description)
{
    assert(!description.empty());
    cmpResult_ = FILE_DIFFERENT_METADATA;
    cmpResultDescr_ = description;
}

inline
void FileSystemObject::flip()
{
    std::swap(itemNameL_, itemNameR_);

    switch (cmpResult_)
    {
        case FILE_LEFT_SIDE_ONLY:
            cmpResult_ = FILE_RIGHT_SIDE_ONLY;
            break;
        case FILE_RIGHT_SIDE_ONLY:
            cmpResult_ = FILE_LEFT_SIDE_ONLY;
            break;
        case FILE_LEFT_NEWER:
            cmpResult_ = FILE_RIGHT_NEWER;
            break;
        case FILE_RIGHT_NEWER:
            cmpResult_ = FILE_LEFT_NEWER;
            break;
        case FILE_DIFFERENT_CONTENT:
        case FILE_EQUAL:
        case FILE_DIFFERENT_METADATA:
        case FILE_CONFLICT:
            break;
    }

    notifySyncCfgChanged();
}


template <SelectSide side> inline
void FileSystemObject::propagateChangedItemName(const Zstring& itemNameOld)
{
    if (itemNameL_.empty() && itemNameR_.empty()) return; //both sides might just have been deleted by removeObject<>

    if (itemNameOld != getItemName<side>()) //perf: premature optimization?
        if (auto hierObj = dynamic_cast<ContainerObject*>(this))
            hierObj->updateRelPathsRecursion<side>(*this);
}


template <SelectSide side> inline
void ContainerObject::updateRelPathsRecursion(const FileSystemObject& fsAlias)
{
    assert(selectParam<side>(relPathL_, relPathR_) != //perf: only call if actual item name changed!
           appendPath(fsAlias.parent().getRelativePath<side>(), fsAlias.getItemName<side>()));

    selectParam<side>(relPathL_, relPathR_) = appendPath(fsAlias.parent().getRelativePath<side>(), fsAlias.getItemName<side>());

    for (FolderPair& folder : subFolders_)
        folder.updateRelPathsRecursion<side>(folder);
}


inline
ContainerObject::ContainerObject(const FileSystemObject& fsAlias) :
    relPathL_(appendPath(fsAlias.parent().relPathL_, fsAlias.getItemName<SelectSide::left>())),
    relPathR_(
        fsAlias.parent().relPathL_.c_str() ==        //
        fsAlias.parent().relPathR_.c_str() &&        //take advantage of FileSystemObject's Zstring reuse:
        fsAlias.getItemName<SelectSide::left >().c_str() == //=> perf: 12% faster merge phase; -4% peak memory
        fsAlias.getItemName<SelectSide::right>().c_str() ?  //
        relPathL_ : //ternary-WTF! (implicit copy-constructor call!!) => no big deal for a Zstring
        appendPath(fsAlias.parent().relPathR_, fsAlias.getItemName<SelectSide::right>())),
    base_(fsAlias.parent().base_)
{
    assert(relPathL_.c_str() == relPathR_.c_str() || relPathL_ != relPathR_);
}


inline
void ContainerObject::flip()
{
    for (FilePair& file : refSubFiles())
        file.flip();
    for (SymlinkPair& symlink : refSubLinks())
        symlink.flip();
    for (FolderPair& folder : refSubFolders())
        folder.flip();

    std::swap(relPathL_, relPathR_);
}


inline
FolderPair& ContainerObject::addFolder(const Zstring& itemNameL,
                                       const FolderAttributes& left,
                                       CompareDirResult defaultCmpResult,
                                       const Zstring& itemNameR,
                                       const FolderAttributes& right)
{
    subFolders_.emplace_back(itemNameL, left, defaultCmpResult, itemNameR, right, *this);
    return subFolders_.back();
}


template <> inline
FolderPair& ContainerObject::addFolder<SelectSide::left>(const Zstring& itemName, const FolderAttributes& attr)
{
    subFolders_.emplace_back(itemName, attr, DIR_LEFT_SIDE_ONLY, Zstring(), FolderAttributes(), *this);
    return subFolders_.back();
}


template <> inline
FolderPair& ContainerObject::addFolder<SelectSide::right>(const Zstring& itemName, const FolderAttributes& attr)
{
    subFolders_.emplace_back(Zstring(), FolderAttributes(), DIR_RIGHT_SIDE_ONLY, itemName, attr, *this);
    return subFolders_.back();
}


inline
FilePair& ContainerObject::addFile(const Zstring&        itemNameL,
                                   const FileAttributes& left,          //file exists on both sides
                                   CompareFileResult     defaultCmpResult,
                                   const Zstring&        itemNameR,
                                   const FileAttributes& right)
{
    subFiles_.emplace_back(itemNameL, left, defaultCmpResult, itemNameR, right, *this);
    return subFiles_.back();
}


template <> inline
FilePair& ContainerObject::addFile<SelectSide::left>(const Zstring& itemName, const FileAttributes& attr)
{
    subFiles_.emplace_back(itemName, attr, FILE_LEFT_SIDE_ONLY, Zstring(), FileAttributes(), *this);
    return subFiles_.back();
}


template <> inline
FilePair& ContainerObject::addFile<SelectSide::right>(const Zstring& itemName, const FileAttributes& attr)
{
    subFiles_.emplace_back(Zstring(), FileAttributes(), FILE_RIGHT_SIDE_ONLY, itemName, attr, *this);
    return subFiles_.back();
}


inline
SymlinkPair& ContainerObject::addLink(const Zstring&        itemNameL,
                                      const LinkAttributes& left, //link exists on both sides
                                      CompareSymlinkResult  defaultCmpResult,
                                      const Zstring&        itemNameR,
                                      const LinkAttributes& right)
{
    subLinks_.emplace_back(itemNameL, left, defaultCmpResult, itemNameR, right, *this);
    return subLinks_.back();
}


template <> inline
SymlinkPair& ContainerObject::addLink<SelectSide::left>(const Zstring& itemName, const LinkAttributes& attr)
{
    subLinks_.emplace_back(itemName, attr, SYMLINK_LEFT_SIDE_ONLY, Zstring(), LinkAttributes(), *this);
    return subLinks_.back();
}


template <> inline
SymlinkPair& ContainerObject::addLink<SelectSide::right>(const Zstring& itemName, const LinkAttributes& attr)
{
    subLinks_.emplace_back(Zstring(), LinkAttributes(), SYMLINK_RIGHT_SIDE_ONLY, itemName, attr, *this);
    return subLinks_.back();
}


inline
void BaseFolderPair::flip()
{
    ContainerObject::flip();
    std::swap(folderStatusLeft_, folderStatusRight_);
    std::swap(folderPathLeft_,   folderPathRight_);
}


inline
void FolderPair::flip()
{
    ContainerObject ::flip(); //call base class versions
    FileSystemObject::flip(); //
    std::swap(attrL_, attrR_);
}


inline
void FolderPair::removeObjectL()
{
    for (FilePair& file : refSubFiles())
        file.removeObject<SelectSide::left>();
    for (SymlinkPair& symlink : refSubLinks())
        symlink.removeObject<SelectSide::left>();
    for (FolderPair& folder : refSubFolders())
        folder.removeObject<SelectSide::left>();

    attrL_ = FolderAttributes();
}


inline
void FolderPair::removeObjectR()
{
    for (FilePair& file : refSubFiles())
        file.removeObject<SelectSide::right>();
    for (SymlinkPair& symlink : refSubLinks())
        symlink.removeObject<SelectSide::right>();
    for (FolderPair& folder : refSubFolders())
        folder.removeObject<SelectSide::right>();

    attrR_ = FolderAttributes();
}


template <SelectSide side> inline
BaseFolderStatus BaseFolderPair::getFolderStatus() const
{
    return selectParam<side>(folderStatusLeft_, folderStatusRight_);
}


template <SelectSide side> inline
void BaseFolderPair::setFolderStatus(BaseFolderStatus value)
{
    selectParam<side>(folderStatusLeft_, folderStatusRight_) = value;
}


inline
void FilePair::flip()
{
    FileSystemObject::flip(); //call base class version
    std::swap(attrL_, attrR_);
}


template <SelectSide side> inline
FileAttributes FilePair::getAttributes() const
{
    return selectParam<side>(attrL_, attrR_);
}


template <SelectSide side> inline
time_t FilePair::getLastWriteTime() const
{
    return selectParam<side>(attrL_, attrR_).modTime;
}


template <SelectSide side> inline
uint64_t FilePair::getFileSize() const
{
    return selectParam<side>(attrL_, attrR_).fileSize;
}


template <SelectSide side> inline
bool FilePair::isFollowedSymlink() const
{
    return selectParam<side>(attrL_, attrR_).isFollowedSymlink;
}


template <SelectSide side> inline
bool FolderPair::isFollowedSymlink() const
{
    return selectParam<side>(attrL_, attrR_).isFollowedSymlink;
}


template <SelectSide side> inline
AFS::FingerPrint FilePair::getFilePrint() const
{
    return selectParam<side>(attrL_, attrR_).filePrint;
}


template <SelectSide side> inline
void FilePair::clearFilePrint()
{
    selectParam<side>(attrL_, attrR_).filePrint = 0;
}


template <SelectSide sideTrg> inline
void FilePair::setSyncedTo(const Zstring& itemName,
                           uint64_t fileSize,
                           int64_t lastWriteTimeTrg,
                           int64_t lastWriteTimeSrc,
                           AFS::FingerPrint filePrintTrg,
                           AFS::FingerPrint filePrintSrc,
                           bool isSymlinkTrg,
                           bool isSymlinkSrc)
{
    //FILE_EQUAL is only allowed for same short name and file size: enforced by this method!
    selectParam<             sideTrg >(attrL_, attrR_) = FileAttributes(lastWriteTimeTrg, fileSize, filePrintTrg, isSymlinkTrg);
    selectParam<getOtherSide<sideTrg>>(attrL_, attrR_) = FileAttributes(lastWriteTimeSrc, fileSize, filePrintSrc, isSymlinkSrc);

    moveFileRef_ = nullptr;
    FileSystemObject::setSynced(itemName); //set FileSystemObject specific part
}


template <SelectSide sideTrg> inline
void SymlinkPair::setSyncedTo(const Zstring& itemName,
                              int64_t lastWriteTimeTrg,
                              int64_t lastWriteTimeSrc)
{
    selectParam<             sideTrg >(attrL_, attrR_) = LinkAttributes(lastWriteTimeTrg);
    selectParam<getOtherSide<sideTrg>>(attrL_, attrR_) = LinkAttributes(lastWriteTimeSrc);

    FileSystemObject::setSynced(itemName); //set FileSystemObject specific part
}


template <SelectSide sideTrg> inline
void FolderPair::setSyncedTo(const Zstring& itemName,
                             bool isSymlinkTrg,
                             bool isSymlinkSrc)
{
    selectParam<             sideTrg >(attrL_, attrR_) = FolderAttributes(isSymlinkTrg);
    selectParam<getOtherSide<sideTrg>>(attrL_, attrR_) = FolderAttributes(isSymlinkSrc);

    FileSystemObject::setSynced(itemName); //set FileSystemObject specific part
}


template <SelectSide side> inline
time_t SymlinkPair::getLastWriteTime() const
{
    return selectParam<side>(attrL_, attrR_).modTime;
}


inline
CompareSymlinkResult SymlinkPair::getLinkCategory() const
{
    return static_cast<CompareSymlinkResult>(getCategory());
}


inline
void SymlinkPair::flip()
{
    FileSystemObject::flip(); //call base class versions
    std::swap(attrL_, attrR_);
}
}

#endif //FILE_HIERARCHY_H_257235289645296
