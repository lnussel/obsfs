#define FUSE_USE_VERSION  26

#define DEBUG_OBSFS

#include <fuse.h>
#include <fuse/fuse_opt.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>

#include <curl/curl.h>
#include <expat.h>
#include <unistd.h>

#include "obsfs.h"
#include "cache.h"
#include "util.h"

#ifdef DEBUG_OBSFS
#define DEBUG(x...) fprintf(stderr, x)
#else
#define DEBUG(x...)
#endif

/* leftovers from the Hello, World example */
static const char *hello_str = "Hello World!\n";
static const char *hello_path = "/hello";

const char *root_dir[] = {
  "/build",
  "/source",
  "/published",
  "/request",
  NULL
};

char *file_cache_dir = NULL;	/* directory to keep cached file contents in */
int file_cache_count = 1;	/* used to make up names for cached files */

char *url_prefix = NULL;	/* prefix for API calls (https://...) */

/* filesystem options */
struct options {
  char *api_username;	/* API user name */
  char *api_password;	/* API user password */
  char *api_hostname;	/* API server name */
} options;

/* lifted from the Hello, World with options example */
#define OBSFS_OPT_KEY(t, p, v) { t, offsetof(struct options, p), v }

static struct fuse_opt obsfs_opts[] =
{
  OBSFS_OPT_KEY("user=%s", api_username, 0),
  OBSFS_OPT_KEY("pass=%s", api_password, 0),
  OBSFS_OPT_KEY("host=%s", api_hostname, 0),
  FUSE_OPT_END
};

static void stat_make_file(struct stat *st)
{
  st->st_mode = S_IFREG | 0644;
  st->st_uid = getuid();
  st->st_gid = getgid();
  st->st_nlink = 1;
}

static void stat_default_file(struct stat *st)
{
  memset(st, 0, sizeof(struct stat));
  stat_make_file(st);
}

static void stat_make_dir(struct stat *st)
{
  st->st_mode = S_IFDIR | 0755;
  st->st_uid = getuid();
  st->st_gid = getgid();
  st->st_nlink = 2;
}

static void stat_default_dir(struct stat *st)
{
  memset(st, 0, sizeof(struct stat));
  stat_make_dir(st);
}

static int obsfs_readdir(const char *path, void *buf, fuse_fill_dir_t filler,
                         off_t offset, struct fuse_file_info *fi);
                         
static int is_in_root_dir(const char *path)
{
  const char **d = root_dir;
  for(; *d; d++) {
    if (!strcmp(*d, path))
      return 1;
  }
  return 0;
}

static int obsfs_getattr(const char *path, struct stat *stbuf)
{
  /* initialize the stat buffer we are going to fill in */
  memset(stbuf, 0, sizeof(struct stat));
  
  if (strcmp(path, "/") == 0 || is_in_root_dir(path)) {
    /* root and the stuff inside it cannot be deduced because the server
       returns a human-readable info page for "/", so they are hardcoded
       here. */
    stat_make_dir(stbuf);
  } else if (strcmp(path, hello_path) == 0) {
    stbuf->st_mode = S_IFREG | 0444;
    stbuf->st_nlink = 1;
    stbuf->st_size = strlen(hello_str);
  } else {
    /* actual API files and directories */
    attr_t *ret;
    DEBUG("getattr: looking for %s\n", path);
    /* let's see if we have that cached already */
    ret = attr_cache_find(path);
    if (ret) {
      DEBUG("found it!\n");
      *stbuf = ret->st;
    } 
    else {
      /* Cache miss, we are going to retrieve the directory "path" is in.
         The reason for that is that the only way to find out about
         a directory entry is to retrieve the entire directory from the
         server. We thus have obsfs_readdir() obtain the directory
         without giving it a filler function, so it will only cache
         the entries it finds in the attribute cache, where we can
         subsequently retrieve the one we're looking for. */
      char *dir = dirname_c(path, NULL);
      DEBUG("not found, trying to get directory\n");
      /* call with buf and filler NULL for cache-only operation */
      obsfs_readdir(dir, NULL, NULL, 0, NULL);
      free(dir);
      /* now the attributes are in the attr cache (if it exists at all) */
      ret = attr_cache_find(path);
      if (ret) {
        DEBUG("found it after all\n");
        *stbuf = ret->st;
      }
      else {
        /* file not found */
        return -ENOENT;
      }
    }
  }

  return 0;
}

static int obsfs_readlink(const char *path, char *buf, size_t buflen)
{
  attr_t *ret = attr_cache_find(path);
  if (ret) {
found:
    if (!ret->symlink)
      return -1;
    strncpy(buf, ret->symlink, buflen - 1);
    buf[buflen-1] = 0;
    return 0;
  }
  char *dir = dirname_c(path, NULL);
  DEBUG("link not found, trying to get directory\n");
  /* call with buf and filler NULL for cache-only operation */
  obsfs_readdir(dir, NULL, NULL, 0, NULL);
  free(dir);
  /* now the attributes are in the attr cache (if it exists at all) */
  ret = attr_cache_find(path);
  if (ret)
   goto found;
  return -1;
}

/* data we need in the expat callbacks to save the directory entries */
struct filbuf {
  void *buf;			/* directory entry buffer, provided by FUSE */
  fuse_fill_dir_t filler;	/* buffer filler function */
  const char *fs_path;		/* directory to read... */
  const char *api_path;		/* ... and where to get it from */
  const char *mangled_path;	/* canonical FS path if fs_path is an alias */
  dir_t *cdir;			/* dir cache entry to fill in */
  int in_dir;			/* flag set when inside a <directory> or <binarylist> */
  const char *filter_attr;
  const char *filter_value;
  const char *relink;
};

/* add a node to a FUSE directory buffer, a directory cache entry, and the attribute cache */
static void add_dir_node(void *buf, fuse_fill_dir_t filler, dir_t *newdir, const char *path, const char *node_name, struct stat *st, const char *symlink, const char *hardlink)
{
  char *full_path;
  /* add node to the directory buffer (if any) */
  if (filler)
    filler(buf, node_name, st, 0);

  /* compose a full path and add node to the attribute cache */
  full_path = malloc(strlen(path) + 1 /* slash */ + strlen(node_name) + 1 /* null */);
  sprintf(full_path, "%s/%s", path, node_name);

  /* Tricky problem: Apparently, FUSE does a LOOKUP (using the getattr
     method) before every open(), but it only does a GETATTR (also using the
     getattr method) the first time a file is opened.  That means that our
     preferred method of updating the file stats in obsfs_open() generally
     works, but if a directory expires and is retrieved from the server
     again, we set the size back to size 0.  When the file is opened now,
     FUSE only does the LOOKUP before open and remembers the wrong file
     size.  The subsequent obsfs_open() call rectifies it for us, but FUSE
     doesn't ask us again and won't permit programs to read any data.  The
     next time the file is opened things are fine again, because the
     previous obsfs_open() run has set the stats correctly, and when FUSE
     does a LOOKUP, it gets the right data and will allow programs to read
     the file.
     To work around this problem, we simply check if we have a cached copy
     already and use its size if so. */

  /* check if we have a local copy that we can use to get the size */
  struct stat local_st;
  if (!lstat(full_path + 1 /* skip leading slash */, &local_st)) {
    st->st_size = local_st.st_size;
  }

  attr_cache_add(full_path, st, symlink, hardlink);
  
  /* add node to the directory cache entry */
  dir_cache_add(newdir, node_name, S_ISDIR(st->st_mode) ? 1 : 0);
  
  if (S_ISDIR(st->st_mode)) {
    attr_t *parent = attr_cache_find(path);
    if (parent)
      parent->st.st_nlink++;
  }
  
  free(full_path);
}

static int endswith(const char *str, const char *end)
{
  return !strcmp(str + strlen(str) - strlen(end), end);
}

/* expat tag start callback for reading API directories */
static void expat_api_dir_start(void *ud, const XML_Char *name, const XML_Char **atts)
{
  struct stat st;
  struct filbuf *fb = (struct filbuf *)ud;

  stat_default_file(&st);

  /* start of directory */
  if (!strcmp(name, "directory") || !strcmp(name, "binarylist") || !strcmp(name, "result")) {
    fb->in_dir = 1;
    return;
  }
  
  /* directory entry */
  if (fb->in_dir && (!strcmp(name, "entry") || !strcmp(name, "binary"))) {
    const char *filename = NULL;
    
    stat_make_dir(&st);	/* assume it's a directory until we know better */
    /* process all attributes */
    while (*atts) {
      if (fb->filter_attr && !strcmp(atts[0], fb->filter_attr) && strcmp(atts[1], fb->filter_value)) {
        filename = NULL;
        break;
      }
      if (!strcmp(atts[0], "name")) {
        /* entry in a "directory" directory; we assume it is itself a directory */
        filename = atts[1];
        /* Muddy waters:
           - There are entries in the /published tree that don't
             have a size, but are files anyway.
           - Everything in /request is a file. */
        if (endswith(filename, ".rpm") || endswith(fb->api_path, "/request"))
          stat_make_file(&st);
      }
      else if (!strcmp(atts[0], "filename")) {
        filename = atts[1];
        /* entry in a "binarylist" directory, this is always a regular file */
        stat_make_file(&st);
      }
      else if (!strcmp(atts[0], "size")) {
        /* file size */
        st.st_size = atoi(atts[1]);
        /* an entry with a size is always a regular file */
        stat_make_file(&st);
      }
      else if (!strcmp(atts[0], "mtime")) {
        st.st_mtime = atoi(atts[1]);
      }
      atts += 2; /* expat hands us a string array with name/value pairs */
    }
    if (filename) {
      char *relink_dir = NULL;
      if (fb->relink) {
        /* have this entry symlink to a file with the same name in a different directory */
        relink_dir = malloc(strlen(fb->relink) + strlen(filename) + 1);
        sprintf(relink_dir, fb->relink, filename);
        //DEBUG("YYYYYYYYYY relinking to %s from %s\n", relink_dir, fb->fs_path);
        st.st_mode = S_IFLNK;
      }
      add_dir_node(fb->buf, fb->filler, fb->cdir, fb->fs_path, filename, &st, relink_dir, NULL);
      if (relink_dir)
        free(relink_dir);
    }
  }
  
  stat_default_file(&st);
  
  if (fb->in_dir && !strcmp(name, "status")) {
    const char *packagename = NULL;
    for (; *atts; atts += 2) {
      if (fb->filter_attr && !strcmp(atts[0], fb->filter_attr) && strcmp(atts[1], fb->filter_value)) {
        packagename = NULL;
        break;
      }
      if (!strcmp(atts[0], "package")) {
        packagename = atts[1];
        stat_make_file(&st);
      }
      /* FIXME: parse endtime */
    }
    if (packagename) {
      /* hardlink to the log file in the package directory */
      char *hardlink = malloc(strlen(fb->mangled_path) + strlen(packagename) + 10);
      
      /* we could either be at build/<project>/_failed/<repo>/<arch> or at
         build/<project>/<repo>/<arch>/_failed, so we use the canonical
         path, which is always the latter */
      strcpy(hardlink, fb->mangled_path);
      
      *(strrchr(hardlink, '/')+1) = 0; /* strip last path element ("_failed") */
      strcat(hardlink, packagename);
      strcat(hardlink, "/_log");
      
      add_dir_node(fb->buf, fb->filler, fb->cdir, fb->fs_path, packagename, &st, NULL, hardlink);

      free(hardlink);
    }
  }
}

/* expat tag end handler for reading API directories */
static void expat_api_dir_end(void *ud, const XML_Char *name)
{
  struct filbuf *fb = (struct filbuf *)ud;
  /* end of API directory */
  if (!strcmp(name, "directory") || !strcmp(name, "binarylist") || !strcmp(name, "result")) {
    fb->in_dir = 0;
  }
}

/* expat expects an fwrite()-style callback, so we need an adapter for XML_Parse() */
static size_t write_adapter(void *ptr, size_t size, size_t nmemb, void *userdata)
{
  XML_Parse((XML_Parser)userdata, ptr, size * nmemb, 0);
  return size * nmemb;
}

/* initialize curl and set API user name and password, writer function and user data */
static CURL *curl_open_file(const char *url, void * read_fun, void *write_fun, void *user_data)
{
  CURL *curl = curl_easy_init();
  curl_easy_setopt(curl, CURLOPT_URL, url);
  curl_easy_setopt(curl, CURLOPT_USERNAME, options.api_username);
  curl_easy_setopt(curl, CURLOPT_PASSWORD, options.api_password);
  curl_easy_setopt(curl, CURLOPT_READFUNCTION, read_fun);
  curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_fun);
  curl_easy_setopt(curl, CURLOPT_READDATA, user_data);
  curl_easy_setopt(curl, CURLOPT_WRITEDATA, user_data);
  curl_easy_setopt(curl, CURLOPT_COOKIEFILE, "cookies"); /* start cookie engine */
  curl_easy_setopt(curl, CURLOPT_COOKIEJAR, "cookies");
  return curl;
}

/* find the depth of a path (used to determine whether it's necessary
   to add additional "fixed" nodes such as _log, _status etc. */
static int path_depth(const char *path)
{
  int count = 0;
  for (path++ /* skip first slash */; *path; path++) {
    if (*path == '/')
      count++;
  }
  return count;
}

static void parse_dir(void *buf, fuse_fill_dir_t filler, dir_t *newdir, const char *fs_path, const char *api_path,
                      const char *mangled_path, const char *filter_attr, const char *filter_value, const char *relink)
{
  char *urlbuf;	/* used to compose the full API URL */
  CURL *curl;
  CURLcode ret;
  XML_Parser xp;
  struct filbuf fb;	/* data the expat callbacks need */
  
  DEBUG("parsing directory %s (API %s)\n", fs_path, api_path);
  
  xp = XML_ParserCreate(NULL);   /* create an expat parser */
  if (!xp)
    abort();

  /* copy some data that the parser callbacks will need */
  fb.filler = filler;
  fb.buf = buf;
  fb.fs_path = fs_path;
  fb.api_path = api_path;
  fb.mangled_path = mangled_path;
  fb.cdir = newdir;
  fb.in_dir = 0;
  fb.filter_attr = filter_attr;
  fb.filter_value = filter_value;
  fb.relink = relink;
  XML_SetUserData(xp, (void *)&fb);	/* pass the data to the parser */

  /* set handlers for start and end tags */
  XML_SetElementHandler(xp, expat_api_dir_start, expat_api_dir_end);
  
  /* construct the full API URL for this directory */
  urlbuf = make_url(url_prefix, api_path);
  
  /* open the URL and set up CURL options */
  curl = curl_open_file(urlbuf, NULL, write_adapter, xp);
  //DEBUG("username %s pw %s\n", options.api_username, options.api_password);
  
  /* perform the actual retrieval; this will instruct curl to get the data from
     the API server and call the write_adapter() for each hunk of data, which will
     in turn call XML_Parse() which will funnel the invidiual components through
     the start and end tag handlers expat_api_dir_start() and expat_api_dir_end() */
  if ((ret = curl_easy_perform(curl))) {
    fprintf(stderr,"curl error %d\n", ret);
  }
  
  /* clean up stuff */
  curl_easy_cleanup(curl);
  XML_ParserFree(xp);
  free(urlbuf);
}

/* string appendectomy: remove "appendix" by copying the non-"appendix"
   parts of "patient" to a new string and returning it */
static char *strstripcpy(char *patient, const char *appendix)
{
  char *ret;
  char *apploc = strstr(patient, appendix);
  if (!apploc)
    return NULL;				/* nothing found */
  ret = malloc(strlen(patient) + 1);		/* the new string is not likely to be larger than the old one */
  strncpy(ret, patient, apploc - patient);	/* copy everything up to "appendix" */
  ret[apploc - patient] = 0;			/* terminate string (in case the following strcat() is a NOP */
  strcat(ret + (apploc - patient), apploc + strlen(appendix));	/* copy stuff after "appendix" */
  return ret;
}

/* read an API directory and fill in the FUSE directory buffer, the directory
   cache, and the attribute cache */
static int get_api_dir(const char *path, void *buf, fuse_fill_dir_t filler)
{
  dirent_t *cached_dirents;
  int cached_dirents_size;
  int mangled_path = 0;
  
  if (filler) {
    filler(buf, ".", NULL, 0);
    filler(buf, "..", NULL, 0);
  }
  
  /* see if we have this directory cached already */
  dir_t *dir = dir_cache_find(path);
  if (dir) {
    cached_dirents = dir->entries;
    cached_dirents_size = dir->num_entries;
    /* cache hit */
    int i;
    struct stat st;

    /* since this dir is already cached, we are done if we don't have a filler */
    if (!filler)
      return 0;

    /* fill the FUSE dir buffer with our cached entries */
    stat_default_file(&st);
    for (i = 0; i < cached_dirents_size; i++) {
      if (cached_dirents[i].is_dir)
        stat_make_dir(&st);
      else
        stat_make_file(&st);
      filler(buf, cached_dirents[i].name, &st, 0);
    }
  }
  else {
    /* not in cache, we have to retrieve it from the API server */
    dir_t *newdir = dir_cache_new(path); /* get directory cache handle */

    char *canon_path = strdup(path);
    char *fpath;
    
    /* handle the build/<project>/_failed/... tree
       This tree collects all the fail logs to make it easier to get
       an overview of failing packages using, for instance, find. */
    if ((fpath = strstr(canon_path, "/_failed"))) {
      char *opath = canon_path;		/* original path requested */
      switch (path_depth(path)) {
      case 2:
      case 3:
        /* build/<project>/_failed and build/<project>/_failed/<foo> are
           equivalent to build/<project> and build/<project>/<foo>, respectively */
        canon_path = strstripcpy(opath, "/_failed");	/* remove the "/_failed" */
        free(opath);
        mangled_path = 1;	/* remember that we messed with the path so we don't add
                                   another "_failed" entry to this directory */
        break;
      case 4:
        /* build/<project>/_failed/<foo>/<bar> is equivalent to
           build/<project>/<foo>/<bar>/_failed */
        canon_path = strstripcpy(opath, "/_failed");	/* remove "/_failed" */
        free(opath);
        strcat(canon_path, "/_failed");		/* ...and add it again at the end */
        mangled_path = 1;
        break;
      default:
        break;
      }
    }
    char *bpath = strdup(canon_path);	/* copy used for strtok_r() dissection */
    /* Is this the (canonical) "_failed" directory? */
    if (!strncmp("/build", canon_path, 6)	/* in the /build tree? */
        && path_depth(canon_path) == 4		/* below <arch> directory? */
        && !strcmp(basename(bpath), "_failed")	/* basename "_failed"? */
       ) {

      /* dissect path to find project, repo, and architecture */
      char *strtokp;
      strtok_r(bpath, "/", &strtokp); /* skip "build" */
      const char *project = strtok_r(NULL, "/", &strtokp);
      const char *repo = strtok_r(NULL, "/", &strtokp);
      const char *arch = strtok_r(NULL, "/", &strtokp);

      /* construct the API server path for "failed" results */
      char *respath = malloc(strlen(project) + strlen(repo) + strlen(arch) + 100 /* too lazy to count right now */);
      sprintf(respath, "/build/%s/_result?repository=%s&arch=%s", project, repo, arch);
      
      /* parse only those entries that have attribute "code" with value "failed" */
      parse_dir(buf, filler, newdir, path, respath, canon_path, "code", "failed", NULL);
      free(respath);
    }
    else {
      /* regular directory, no special handling */
      parse_dir(buf, filler, newdir, path, canon_path, canon_path, NULL, NULL, NULL);
    }
    free(bpath);
    free(canon_path);
    
    /* check if we need to add additional nodes */
    /* Most of the available API is not exposed through directories. We have to know
       about it and add them ourselves at the appropriate places. */
    DEBUG("path depth of %s is %d\n", path, path_depth(path));
    
    /* special entries for /build tree */
    if (!mangled_path			/* no additional nodes if we have messed with the path */
        && !strncmp("/build", path, 6)
       ) {
      /* "_failed" directories */
      if (path_depth(path) == 3 || path_depth(path) == 1) {
        /* build/<project>/<repo>/<arch>/_failed and build/_failed */
        struct stat st;
        stat_default_dir(&st);
        add_dir_node(buf, filler, newdir, path, "_failed", &st, NULL, NULL);
      }
      /* log, history, status, and reason for packages */
      char *fpath = strdup(path);
      if (path_depth(path) == 4			/* build/.../.../.../... */
          && strcmp(basename(fpath), "_failed") /* but not build/.../.../.../_failed */
         ) {
        int i;
        struct stat st;

        stat_default_file(&st);
        /* st.st_size = 4096; not sure if this is a good idea;
           this entry is corrected to reflect the actual size
           when the file is treated by obsfs_open() */
        
        /* package status APIs */
        const char const *status_api[] = {
          "_history", "_reason", "_status", "_log", NULL
        };
        for (i = 0; status_api[i]; i++) {
          add_dir_node(buf, filler, newdir, path, status_api[i], &st, NULL, NULL);
        }
      }
      free(fpath);
    }
  }
  return 0;
}

static int obsfs_readdir(const char *path, void *buf, fuse_fill_dir_t filler,
                         off_t offset, struct fuse_file_info *fi)
{
  (void)offset;
  (void)fi;
  struct stat st;

  stat_default_file(&st);
  DEBUG("readdir path %s\n", path);
  
  /* The API server does not provide us with a root directory; retrieving
     "/" only yields a human-readable info page. We therefore have to
     construct the root directory manually. */
  if (!strcmp(path, "/") && filler && buf) {
    filler(buf, ".", NULL, 0);
    filler(buf, "..", NULL, 0);
    filler(buf, hello_path + 1, NULL, 0);
    st.st_mode = S_IFDIR;
    const char **d;
    /* fill in the root directory entries */
    for(d = root_dir; *d; d++) {
      filler(buf, (*d)+1 /* skip slash */, &st, 0);
    }
  }
  
  /* If it's not the root directory, we get it from the API server (or dir cache). */
  return get_api_dir(path, buf, filler);
}

/* retrieve a file, store it in our local file cache, and return a descriptor
   to the local copy */
static int obsfs_open(const char *path, struct fuse_file_info *fi)
{
  char *urlbuf;
  CURL *curl;
  FILE *fp;
  CURLcode ret;
  struct stat st;
  const char *relpath = path + 1; /* skip leading slash */
  attr_t *at = attr_cache_find(path);
  
  /* leftovers from Hello, World example */
  if (strcmp(path, hello_path) == 0) {
    if ((fi->flags & 3) != O_RDONLY)
      return -EACCES;
    fi->fh = 0;
    return 0;
  }

  fp = fopen(relpath, "r+");
  if (!fp) {
    /* create the cache file */
    if (mkdirp(relpath, 0755))
      return -1;
    fp = fopen(relpath, "w+");
    if (!fp)
      return -1;
  
    /* find out if this file is supposed to hardlink somewhere */
    const char *effective_path = path;
    if (at) {
      if (at->hardlink) {
        //DEBUG("BBBBBBBBB %s hardlinks to %s\n", path, a->hardlink);
        effective_path = at->hardlink;
      }
    }

    /* compose the full URL */
    urlbuf = make_url(url_prefix, effective_path);
    
    /* retrieve the file from the API server */
    curl = curl_open_file(urlbuf, NULL, fwrite, fp);
    if ((ret = curl_easy_perform(curl))) {
      fprintf(stderr,"curl error %d\n", ret);
    }
    curl_easy_cleanup(curl);
    
    free(urlbuf);
  }
  
  /* create a new file handle for the cache file, we need it later to retrieve
     the contents (it's unlinked already) */
  fi->fh = dup(fileno(fp));
  fclose(fp);

  /* now that we have the actual size, update the stat cache; this is necessary
     for the special nodes, the sizes of which we don't know when constructing
     their directory entries */
  if (fstat(fi->fh, &st)) {
    perror("fstat");
  }
  attr_cache_add(path, &st, at? at->symlink : NULL, at? at->hardlink : NULL);

  return 0;
}

static int obsfs_read(const char *path, char *buf, size_t size, off_t offset,
                      struct fuse_file_info *fi)
{
  size_t len;
  /* Hello, World leftover */
  if (strcmp(path, hello_path) == 0) {
    len = strlen(hello_str);
    if (offset < len) {
      if (offset + size > len)
        size = len - offset;
      memcpy(buf, hello_str + offset, size);
    } else
      size = 0;

    return size;
  }
  
  /* read from the cache file */
  int ret = pread(fi->fh, buf, size, offset);
  if (ret < 0)
    return -errno;
  else
    return ret;
}

static int obsfs_write(const char *path, const char *buf, size_t size, off_t offset,
                       struct fuse_file_info *fi)
{
  attr_t *at = attr_cache_find(path);
  if (!at) {
    DEBUG("WRITE: internal error writing to %s\n", path);
    return -1;
  }
  if (!at->modified) {
    at->modified = 1;
    char *dn = dirname_c(path, NULL);
    dir_t *dir = dir_cache_find(dn);
    free(dn);
    if (dir) {
      dir->modified++;
    }
  }
  if (offset + size > at->st.st_size)
    at->st.st_size = offset + size;
  return pwrite(fi->fh, buf, size, offset);
}

static int obsfs_truncate(const char *path, off_t offset)
{
  return truncate(path + 1, offset);
}

static int obsfs_flush(const char *path, struct fuse_file_info *fi)
{
  int ret;
  FILE *fp;
  DEBUG("FLUSH: flushing %s\n", path);
  
  /* If the file is being flushed, we have seen it before, so it's in the attr cache. */
  /* FIXME: What if it has expired there? */
  attr_t *at = attr_cache_find(path);
  if (!at) {
    DEBUG("FLUSH: internal error flushing %s\n", path);
    return -1;
  }
  
  /* If it has been modified, we need to write it back to the API server. */
  if (at->modified) {
    /* where to PUT it */
    char *url = make_url(url_prefix, path);
    
    if (lseek(fi->fh, 0, SEEK_SET) < 0)
      return -errno;
      
    /* curl likes fread(), so we get us a FILE pointer */
    fp = fdopen(dup(fi->fh), "r");
    if (!fp) {
      perror("fdopen");
      return -errno;
    }
    
    /* prepare for uploading the file */
    CURL *curl = curl_open_file(url, fread, NULL, fp);
    curl_easy_setopt(curl, CURLOPT_UPLOAD, 1);
    
    /* need to tell curl about the file size */
    struct stat st;
    fstat(fi->fh, &st);
    curl_easy_setopt(curl, CURLOPT_INFILESIZE_LARGE, st.st_size);
    
    /* do it! */
    if ((ret = curl_easy_perform(curl))) {
      fclose(fp);
      fprintf(stderr,"curl error %d\n", ret);
      return -1;
    }
    curl_easy_cleanup(curl);
    fclose(fp);
    
    at->modified = 0;
    char *dn = dirname_c(path, NULL);
    dir_t *dir = dir_cache_find(dn);
    free(dn);
    if (dir) {
      dir->modified--;
    }
  }
  return 0;
}

static int obsfs_create(const char *path, mode_t mode, struct fuse_file_info *fi)
{
  struct stat st;
  DEBUG("CREATE %s\n", path);
  
  /* create a new cache file */
  mkdirp(path + 1, 0755);
  if ((fi->fh = open(path + 1 /* skip slash */, O_CREAT|O_RDWR|O_TRUNC, mode)) < 0)
    return -errno;
  
  /* create a new attr cache entry for that file */
  stat_default_file(&st);
  st.st_mode = mode;
  attr_cache_add(path, &st, NULL, NULL);
  
  /* add it to its directory in the cache */
  /* FIXME: It won't appear in the upstream directory until the next flush,
     might cause inconsistencies. */
  char *bn, *dn;
  dn = dirname_c(path, &bn);
  dir_t *dir = dir_cache_find(dn);
  if (dir) {
    dir_cache_add(dir, bn, 0);
    /* FIXME: We should increment dir->modified here, but we can't because
       we don't set the modified flag in the newly created attribute so as
       not to sync an empty file needlessly, so dir->modified would never be
       reset...  */
  }
  free(dn);
  
  return 0;
}

/* Giving it a NULL pointer as the reader function doesn't deter curl from
   using fwrite() anyway, so we need this expceptionally useless function. */
static size_t write_null(void *ptr, size_t size, size_t nmemb, void *userdata)
{
  (void)ptr;
  (void)userdata;
  return size * nmemb;
}

static int obsfs_unlink(const char *path)
{
  int ret, cret;
  int rerrno;		/* errno set by unlink() */
  DEBUG("UNLINK %s\n", path);
  
  /* remove node from the attribute and directory caches */
  attr_cache_remove(path);
  dir_cache_remove(path);
  
  /* remove node from file cache */
  ret = unlink(path + 1);
  rerrno = errno;
  
  /* remove node from server */
  char *url = make_url(url_prefix, path);
  CURL *curl = curl_open_file(url, NULL, write_null, NULL);
  curl_easy_setopt(curl, CURLOPT_CUSTOMREQUEST, "DELETE");
  
  if ((cret = curl_easy_perform(curl))) {
    DEBUG("UNLINK: curl error %d\n", cret);
    if (ret) {
      /* both unlink() in the cache and DELETE on the server failed */
      return -rerrno;
    }
  }
  /* if either unlink() or DELETE on the server worked OK, we're fine */
  return 0;
}

static void *obsfs_init(struct fuse_conn_info *conn)
{
  /* change to the file cache directory; that way we don't have to remember it elsewhere */
  if (chdir(file_cache_dir)) {
    perror("chdir");
    abort();
  }

  /* construct an URL prefix from API server host name, user name and password */
  const char *host;
  if (options.api_hostname)
    host = options.api_hostname;
  else
    host = "api.opensuse.org";
  url_prefix = malloc(strlen(host) + strlen("https://") + 1);
  sprintf(url_prefix, "https://%s", host);

  return NULL;
}

static void obsfs_destroy(void *foo)
{
  free(url_prefix);
}

static struct fuse_operations obsfs_oper = {
  .init = obsfs_init,
  .destroy = obsfs_destroy,
  .getattr = obsfs_getattr,
  .readdir = obsfs_readdir,
  .open = obsfs_open,
  .flush = obsfs_flush,
  .truncate = obsfs_truncate,
  .create = obsfs_create,
  .read = obsfs_read,
  .write = obsfs_write,
  .readlink = obsfs_readlink,
  .unlink = obsfs_unlink,
};

int main(int argc, char *argv[])
{
  int ret;

  /* parse filesystem options */
  struct fuse_args args = FUSE_ARGS_INIT(argc, argv);
  args.argv[args.argc++] = "-o";
  args.argv[args.argc++] = "attr_timeout=0";
  args.argv[args.argc] = NULL;
  
  memset(&options, 0, sizeof(struct options));
  if (fuse_opt_parse(&args, &options, obsfs_opts, NULL) == -1)
    return -1;

  /* initialize libcurl */
  if (curl_global_init(CURL_GLOBAL_ALL))
    return -1;

  /* initialize caches */
  attr_cache_init();
  dir_cache_init();
  
  /* create a directory for the file cache */
  file_cache_dir = strdup("/tmp/obsfs_cacheXXXXXX");
  if (!mkdtemp(file_cache_dir)) {
    perror("mkdtemp");
    return -1;
  }
  /* can't do the chdir() here because we might have a relative
     mount point specified; will do it in obsfs_init() */

  /* Go! */
  ret = fuse_main(args.argc, args.argv, &obsfs_oper, NULL);
  
  /* remove the file cache */
  if (!chdir(file_cache_dir)) {
    system("rm -fr *");
  }
  if (rmdir(file_cache_dir)) {
    perror("rmdir");
  }
  free(file_cache_dir);
  
  fuse_opt_free_args(&args);
  attr_cache_free();
  dir_cache_free();
  
  return ret;
}
