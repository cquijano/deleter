#include <stdio.h>
#include <stdlib.h>
#include <syslog.h>
#include <dirent.h> 
#include <mntent.h>
#include <sys/vfs.h>
#include <sys/types.h> 
#include <sys/stat.h> 
#include <fcntl.h>
#include <unistd.h> 
#include <string.h>
#include <stdlib.h>
#include <stdbool.h>
#include <errno.h>
#include <sys/inotify.h>
#include <time.h>

#define DAEMON

#define INFO(...) syslog(LOG_INFO, __VA_ARGS__)
#define ERR(...) syslog(LOG_ERR, __VA_ARGS__)
#define BUG(...) syslog(LOG_CRIT, __VA_ARGS__)

#ifdef DAEMON
	#define DEBUG(...) syslog(LOG_CRIT, __VA_ARGS__)
#else
	#define DEBUG(...) printf( __VA_ARGS__)
#endif

#define PID_FILE "/tmp/deleter.pid"
static int lfp =0;

void daemonize(void)
{
#ifndef DAEMON
	return;
#else
	int i;
	char str[10];
	
	if(getppid()==1) 
		return;
	if (fork()) exit(0);
	setsid();
	for (i=getdtablesize();i>=0;--i) 
		close(i); 
	i=open("/dev/null",O_RDWR); 
	dup(i); 
	dup(i);
	
	lfp=open(PID_FILE,O_RDWR|O_CREAT,0640);
	if (lfp<0) 
		exit(1); /* can not open */
	if (lockf(lfp,F_TLOCK,0)<0) 
		exit(0); /* can not lock */
	/* first instance continues */
	sprintf(str,"%d\n",getpid());
	write(lfp,str,strlen(str)); /* record pid to lockfile */
#endif
}

/**Return percent usage*/
int get_free_space(const char* path){
	struct statfs s;
	unsigned long blocks_used;
	unsigned blocks_percent_used;
	if (statfs(path, &s)) {
		return -1;
	}
	blocks_used = s.f_blocks - s.f_bfree;
	blocks_percent_used = 0;
	if (blocks_used + s.f_bavail) {
		blocks_percent_used = (blocks_used * 100ULL
				+ (blocks_used + s.f_bavail)/2
				) / (blocks_used + s.f_bavail);
	}
	DEBUG("Space used %u\n",blocks_percent_used);
	DEBUG("free Space used %u\n",100-blocks_percent_used);
	return (100-blocks_percent_used);
}

int make_file_path(const char* path,const char* file_name, char* abs_path){
	if (path[strlen(path)-1]=='/')
		return (sprintf(abs_path,"%s%s",path,file_name));
	else{
		return (sprintf(abs_path,"%s/%s",path,file_name));
	}
}

static time_t file_time=0;
static char older_file[256];

time_t get_older_file (const char* path,char* buf){
	DIR           *d;
	struct dirent *dir;
	struct stat s;
	char file[256];
	
	
	if (!file_time)
		file_time=time(0);
	
	d = opendir(path);
	if (d){
		DEBUG("\tDir oppened [%s]\n\n",path);
		while ((dir = readdir(d)) != NULL){
			if (!strncmp(".",dir->d_name,1) || !strncmp("..",dir->d_name,2))
				continue;
			make_file_path(path,dir->d_name,file);
			if (stat(file,&s))
				continue;
			if (S_ISDIR(s.st_mode)){
				file_time=get_older_file(file,NULL);
				if (!file_time)
					continue;
			}else if (s.st_mtime<file_time){
				file_time=s.st_mtime;
				strcpy(older_file,file);
				DEBUG("OLDER [%s]",file);
				DEBUG(" Time is [%li]",s.st_mtime);
				DEBUG(" Size is [%li]\n\n",s.st_size);
			}
		}
		closedir(d);
	}else{
		ERR("No Dir\n");
		return 0;
	}
	if (buf){
		strcpy(buf,older_file);
	}
	return file_time;
}

/**return if directory is empty or not*/
bool clean_empty_dir(const char* path){
	DIR           *d;
	struct dirent *dir;
	struct stat s;
	char file[256];
	bool empty=true;
	
	d = opendir(path);
	if (d){
		DEBUG("Clear Dir  [%s]\n",path);
		DEBUG("Dir oppened [%s]\n",path);
		while ((dir = readdir(d)) != NULL){
			if (!strncmp(".",dir->d_name,1) || !strncmp("..",dir->d_name,2))
				continue;
			make_file_path(path,dir->d_name,file);
			if (stat(file,&s))
				continue;
			if (S_ISDIR(s.st_mode)){
				DEBUG("Doing recursive Callback [%s]\n",file);
				clean_empty_dir(file);
			}else{
				empty=false;
			}
		}
		closedir(d);
		if (empty){/*empty directory*/
			DEBUG("Dir is [%s] empty\n", path);
			if (rmdir(path)){
				ERR("Rmdir Error %i, %s\n", errno, strerror(errno));
			}
			return 0;
		}
	}else{
		ERR("No Dir\n");
		return 0;
	}
	return empty;
}

time_t free_device (const char* path, int percent_to_get_free){

	char file[256];

	DEBUG("Freeing space on %s\n",path);
	while (get_free_space(path)<percent_to_get_free){
		get_older_file(path,file);
		DEBUG("Deleting %s\n",file);
		file_time=0;
#ifndef TEST
		unlink(file);
#else
		DEBUG("Deleting File %s\n",file);
		exit(0);
#endif
	}
	return 0;
}
static int percent=10;
typedef int (*callback)(const char*);

#define EVENT_SIZE  ( sizeof (struct inotify_event) )
#define EVENT_BUF_LEN     ( 10 * ( EVENT_SIZE + 16 ) )

int wait_for_write_fs(const char* path, callback ptr_callback)
{
	int length, i = 0;
	int fd;
	int wd;
	char buffer[EVENT_BUF_LEN];
	struct inotify_event *event;

	fd = inotify_init();

	if ( fd < 0 ) {
		ERR( "Inotify init Error\n" );
	}

	wd = inotify_add_watch( fd, path, IN_CLOSE_WRITE );
	while (1){
		DEBUG("Waiting for inotify event\n");
		length = read( fd, buffer, EVENT_BUF_LEN );

		if ( length < 0 ) {
			ERR( "Error on Read" );
			break;
		}  

		while ( i < length ) {     
			event = ( struct inotify_event * ) &buffer[ i ];     
			if ( event->len ) {
				DEBUG( "New File %s Writed.\n", event->name );
				ptr_callback(path);
			}
			i += EVENT_SIZE + event->len;
		}
		i=0;
	}
	
	inotify_rm_watch( fd, wd );

	close( fd );
	return 0;
}

int clean_fs(const char* path){
	DEBUG("Cleaning FS %s\n",path);
	free_device(path,percent);
	clean_empty_dir(path);
	return 0;
}

int check_values(const char* path,const char* strpercent){
	percent= atoi(strpercent);
	if (percent<0 || percent > 99)
		return 0;/*bat argument*/
	struct stat s;
	if (stat(path, &s)) {
		return 0;/*file not exits*/
	}
	return 1;
}

void print_usage(const char* arg){
	printf("%s path_to_file percent\n",arg);
}

int main(int argc, char **argv){
	if ( !argv[1] || !argv[2] ){
		print_usage(argv[0]);
		exit(-1);
	}
	if (!check_values(argv[1],argv[2])){
		print_usage(argv[0]);
		exit(-1);
	}
	
	openlog("deleter",LOG_ERR, LOG_DAEMON);
	daemonize();
	/**At begining clean fs*/
	clean_fs(argv[1]);
	return (wait_for_write_fs(argv[1],(callback)clean_fs));
}