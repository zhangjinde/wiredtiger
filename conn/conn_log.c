/*************************************************************************
*conn redo log初始化、相关线程控制等
*************************************************************************/

#include "wt_internal.h"

/*从配置字符串中解析transaction_sync对应的配置项目*/
static int __logmgr_sync_cfg(WT_SESSION_IMPL* session, const char** cfg)
{
	WT_CONFIG_ITEM cval;
	WT_CONNECTION_IMPL *conn;

	conn = S2C(session);

	/*读取txn sync的开启标识*/
	WT_RET(__wt_config_gets(session, cfg, "transaction_sync.enabled", &cval));

	/*设置事务日志的标识，用于redo log的flush*/
	if(cval.val)
		FLD_SET(conn->txn_logsync, WT_LOG_FLUSH);
	else
		FLD_CLR(conn->txn_logsync, WT_LOG_FLUSH);

	WT_RET(__wt_config_gets(session, cfg, "transaction_sync.method", &cval));
	/*将WT_LOG_DSYNC标识复位清除,并从配置项上读取是否有配置FSYNC和DSYNC*/
	FLD_CLR(conn->txn_logsync, WT_LOG_DSYNC | WT_LOG_FSYNC);
	if (WT_STRING_MATCH("dsync", cval.str, cval.len))
		FLD_SET(conn->txn_logsync, WT_LOG_DSYNC);
	else if (WT_STRING_MATCH("fsync", cval.str, cval.len))
		FLD_SET(conn->txn_logsync, WT_LOG_FSYNC);

	return 0;
}

/*解析并设置log相关的日志配置选项值*/
static int __logmgr_config(WT_SESSION_IMPL* session, const char** cfg, int* runp)
{
	WT_CONFIG_ITEM cval;
	WT_CONNECTION_IMPL *conn;

	conn = S2C(session);

	/*日志开启开关，默认是关闭的*/
	WT_RET(__wt_config_gets(session, cfg, "log.enabled", &cval));
	*runp = cval.val != 0;

	/*读取日志是否进行压缩项目*/
	conn->log_compressor = NULL;
	WT_RET(__wt_config_gets_none(session, cfg, "log.compressor", &cval));
	WT_RET(__wt_compressor_config(session, &cval, &conn->log_compressor));

	/*读取日志文件存放的路径*/
	WT_RET(__wt_config_gets(session, cfg, "log.path", &cval));
	WT_RET(__wt_strndup(session, cval.str, cval.len, &conn->log_path));

	if (*runp == 0)
		return (0);

	/*读取log archive标识*/
	WT_RET(__wt_config_gets(session, cfg, "log.archive", &cval));
	if (cval.val != 0)
		FLD_SET(conn->log_flags, WT_CONN_LOG_ARCHIVE);

	/*获得日志文件最大空间大小*/
	WT_RET(__wt_config_gets(session, cfg, "log.file_max", &cval));
	conn->log_file_max = (wt_off_t)cval.val;
	WT_STAT_FAST_CONN_SET(session, log_max_filesize, conn->log_file_max);

	/*获得日志的预分配配置项*/
	WT_RET(__wt_config_gets(session, cfg, "log.prealloc", &cval));
	if (cval.val != 0) {
		FLD_SET(conn->log_flags, WT_CONN_LOG_PREALLOC);
		conn->log_prealloc = 1;
	}

	/*读取日志推演的选项*/
	WT_RET(__wt_config_gets_def(session, cfg, "log.recover", 0, &cval));
	if (cval.len != 0  && WT_STRING_MATCH("error", cval.str, cval.len))
		FLD_SET(conn->log_flags, WT_CONN_LOG_RECOVER_ERR);

	WT_RET(__logmgr_sync_cfg(session, cfg));

	return 0;
}

/*进行一次日志归档操作,相当于删除多余的日志文件*/
static int __log_archive_once(WT_SESSION_IMPL *session, uint32_t backup_file)
{
	WT_CONNECTION_IMPL *conn;
	WT_DECL_RET;
	WT_LOG *log;
	uint32_t lognum, min_lognum;
	u_int i, locked, logcount;
	char **logfiles;

	conn = S2C(session);
	log = conn->log;
	logcount = 0;
	logfiles = NULL;

	/*一定是从小于checkpoint处进行归档的，如果从大于checkpoint处去归档，那么很有可能会丢失数据*/
	if (backup_file != 0)
		min_lognum = WT_MIN(log->ckpt_lsn.file, backup_file);
	else
		min_lognum = WT_MIN(log->ckpt_lsn.file, log->sync_lsn.file);

	WT_RET(__wt_verbose(session, WT_VERB_LOG, "log_archive: archive to log number %" PRIu32, min_lognum));

	/*获得log目录下的日志文件名列表*/
	WT_RET(__wt_dirlist(session, conn->log_path, WT_LOG_FILENAME, WT_DIRLIST_INCLUDE, &logfiles, &logcount));

	__wt_spin_lock(session, &conn->hot_backup_lock);
	locked = 1;
	if(conn->hot_backup == 0 || backup_file != 0){
		for (i = 0; i < logcount; i++) {
			WT_ERR(__wt_log_extract_lognum(session, logfiles[i], &lognum));
			if (lognum < min_lognum)
				WT_ERR(__wt_log_remove(session, WT_LOG_FILENAME, lognum)); 
			/*删除要归档的日志文件,这个地方直接删除，会不会不妥，innobase里面是将文件
			 *备份到一个目录，定时拷贝走*/
		}
	}

	/*正常结束，进行资源释放*/
	__wt_spin_unlock(session, &conn->hot_backup_lock);
	locked = 0;
	__wt_log_files_free(session, logfiles, logcount);
	logfiles = NULL;
	logcount = 0;

	log->first_lsn.file = min_lognum;
	log->first_lsn.offset = 0;

	return ret;

	/*错误处理*/
err:
	__wt_err(session, ret, "log archive server error");
	if (locked)
		__wt_spin_unlock(session, &conn->hot_backup_lock);

	if (logfiles != NULL)
		__wt_log_files_free(session, logfiles, logcount);

	return ret;
}

/*进行一次日志文件预分配*/
static int __log_prealloc_once(WT_SESSION_IMPL* session)
{
	WT_CONNECTION_IMPL *conn;
	WT_DECL_RET;
	WT_LOG *log;
	u_int i, reccount;
	char **recfiles;

	conn = S2C(session);
	log = conn->log;
	reccount = 0;
	recfiles = NULL;

	/*计算已经在log目录下存在的预分配文件数量*/
	WT_ERR(__wt_dirlist(session, conn->log_path, WT_LOG_PREPNAME, WT_DIRLIST_INCLUDE, &recfiles, &reccount));
	__wt_log_files_free(session, recfiles, reccount);
	recfiles = NULL;

	/*假如以前有预分配文件重复利用了已经存在的预分配文件，那么将重复利用的次数 和预分配的文件数相加，作为本次预分配的文件数*/
	if (log->prep_missed > 0) {
		conn->log_prealloc += log->prep_missed;
		WT_ERR(__wt_verbose(session, WT_VERB_LOG, "Now pre-allocating up to %" PRIu32, conn->log_prealloc));
		log->prep_missed = 0;
	}

	WT_STAT_FAST_CONN_SET(session, log_prealloc_max, conn->log_prealloc);
	/*建立预分配日志文件*/
	for (i = reccount; i < (u_int)(conn->log_prealloc); i++) {
		WT_ERR(__wt_log_allocfile(session, ++log->prep_fileid, WT_LOG_PREPNAME, 1));
		WT_STAT_FAST_CONN_INCR(session, log_prealloc_files);
	}

	return ret;

err:
	__wt_err(session, ret, "log pre-alloc server error");
	if (recfiles != NULL)
		__wt_log_files_free(session, recfiles, reccount);

	return ret;
}

int __wt_log_truncate_files(WT_SESSION_IMPL *session, WT_CURSOR *cursor, const char *cfg[])
{
	WT_CONNECTION_IMPL *conn;
	WT_DECL_RET;
	WT_LOG *log;
	uint32_t backup_file, locked;

	WT_UNUSED(cfg);
	conn = S2C(session);
	log = conn->log;

	if (F_ISSET(conn, WT_CONN_SERVER_RUN) && FLD_ISSET(conn->log_flags, WT_CONN_LOG_ARCHIVE))
		WT_RET_MSG(session, EINVAL, "Attempt to archive manually while a server is running");

	/*确定归档的最大日志文件序号*/
	backup_file = 0;
	if (cursor != NULL)
		backup_file = WT_CURSOR_BACKUP_ID(cursor);

	WT_ASSERT(session, backup_file <= log->alloc_lsn.file);
	WT_RET(__wt_verbose(session, WT_VERB_LOG, "log_truncate_files: Archive once up to %" PRIu32, backup_file));

	/*进行日志归档,这里用的是一个读写锁？？*/
	WT_RET(__wt_writelock(session, log->log_archive_lock));
	locked = 1;
	WT_ERR(__log_archive_once(session, backup_file));
	WT_ERR(__wt_writeunlock(session, log->log_archive_lock));
	locked = 0;

err:
	if(locked)
		WT_RET(__wt_writeunlock(session, log->log_archive_lock));

	return ret;
}

/*对close_fh对应的文件进行fsync和关闭操作,一般只有等到log_close_cond信号触发才会进行一次close操作检查,是一个线程体函数*/
static WT_THREAD_RET __log_close_server(void* arg)
{
	WT_CONNECTION_IMPL *conn;
	WT_DECL_RET;
	WT_FH *close_fh;
	WT_LOG *log;
	WT_LSN close_end_lsn, close_lsn;
	WT_SESSION_IMPL *session;
	int locked;

	session = arg;
	conn = S2C(session);
	log = conn->log;
	locked = 0;

	while(F_ISSET(conn, WT_CONN_LOG_SERVER_RUN)){
		/*close fh中有文件等待close操作，并且文件末尾对应的LSN位置已经小于正在写日志的文件LSN,表明这个文件不可能再支持写，可以关闭*/
		close_fh = log->log_close_fh;
		if (close_fh != NULL && 
			(ret = __wt_log_extract_lognum(session, close_fh->name, &close_lsn.file)) == 0 && close_lsn.file < log->write_lsn.file) {

				log->log_close_fh = NULL;

				close_lsn.offset = 0;
				close_end_lsn = close_lsn;
				close_end_lsn.file++;

				/*进行fsync操作*/
				WT_ERR(__wt_fsync(session, close_fh));
				__wt_spin_lock(session, &log->log_sync_lock);

				/*关闭文件*/
				locked = 1;
				WT_ERR(__wt_close(session, &close_fh));
				log->sync_lsn = close_end_lsn;
				/*触发一个log_sync_cond表示sync_lsn重新设置了新的值*/
				WT_ERR(__wt_cond_signal(session, log->log_sync_cond));
				locked = 0;

				__wt_spin_unlock(session, &log->log_sync_lock);
		}
		else{
			/*等待下一次文件的close cond*/
			WT_ERR(__wt_cond_wait(session, conn->log_close_cond, WT_MILLION));
		}
	}

	return WT_THREAD_RET_VALUE;

err:
	__wt_err(session, ret, "log close server error");
	if (locked)
		__wt_spin_unlock(session, &log->log_sync_lock);

	return WT_THREAD_RET_VALUE;
}

typedef struct {
	WT_LSN		lsn;
	uint32_t	slot_index;
} WT_LOG_WRLSN_ENTRY;

/*WT_LOG_WRLSN_ENTRY的比较器函数,其实他们的lsn的比较，为了保证lsn顺序所写的*/
static int WT_CDECL __log_wrlsn_cmp(const void *a, const void *b)
{
	WT_LOG_WRLSN_ENTRY *ae, *be;

	ae = (WT_LOG_WRLSN_ENTRY *)a;
	be = (WT_LOG_WRLSN_ENTRY *)b;
	return LOG_CMP(&ae->lsn, &be->lsn);
}

/*只是处理SLOT_BUFFERED且不主动的fsync的模式*/
static WT_THREAD_RET __log_wrlsn_server(void* arg)
{
	WT_CONNECTION_IMPL *conn;
	WT_DECL_RET;
	WT_LOG*				log;
	WT_LOG_WRLSN_ENTRY	written[SLOT_POOL];
	WT_LOGSLOT*			slot;
	WT_SESSION_IMPL*	session;
	size_t				written_i;
	uint32_t			i, save_i;
	int					yield;

	session = (WT_SESSION_IMPL*)arg;
	conn = S2C(session);
	log = conn->log;
	yield = 0;

	while(F_ISSET(conn, WT_CONN_LOG_SERVER_RUN)){

		i = 0;
		written_i = 0;

		/*这里不需要对slot pool进行多线程保护，因为slot pool是个静态的数组*/
		while(i < SLOT_POOL){
			save_i = i;
			slot = &log->slot_pool[i++];
			if (slot->slot_state != WT_LOG_SLOT_WRITTEN) /*过滤掉非WRITTEN状态*/
				continue;

			written[written_i].slot_index = save_i;
			written[written_i++].lsn = slot->slot_release_lsn;
		}

		if (written_i > 0) {
			/*触发一次，表示后续可能很多这样的操作，所以讲yield进行操作*/
			yield = 0;
			/*按LSN由小到大排序,因为要按slot进行数据刷盘*/
			qsort(written, written_i, sizeof(WT_LOG_WRLSN_ENTRY), __log_wrlsn_cmp);
			/*
			 * We know the written array is sorted by LSN.  Go
			 * through them either advancing write_lsn or stop
			 * as soon as one is not in order.
			 */
			for (i = 0; i < written_i; i++) {
				if (LOG_CMP(&log->write_lsn, &written[i].lsn) != 0)
					break;
				/*
				 * If we get here we have a slot to process.
				 * Advance the LSN and process the slot.
				 */
				slot = &log->slot_pool[written[i].slot_index];
				WT_ASSERT(session, LOG_CMP(&written[i].lsn, &slot->slot_release_lsn) == 0);
				/*更新WRITE LSN*/
				log->write_lsn = slot->slot_end_lsn;
				WT_ERR(__wt_cond_signal(session,log->log_write_cond));

				WT_STAT_FAST_CONN_INCR(session, log_write_lsn);

				/*
				 * Signal the close thread if needed.尝试把file page cache的数据sync到磁盘上
				 */
				if (F_ISSET(slot, SLOT_CLOSEFH))
					WT_ERR(__wt_cond_signal(session, conn->log_close_cond));

				WT_ERR(__wt_log_slot_free(session, slot));
			}
		}

		if (yield++ < 1000)
			__wt_yield();
		else
			/* Wait until the next event. */
			WT_ERR(__wt_cond_wait(session, conn->log_wrlsn_cond, 100000));
	}

	return WT_THREAD_RET_VALUE;

err:
	__wt_err(session, ret, "log wrlsn server error");

	return WT_THREAD_RET_VALUE;
}

/*一个专门删除已经建立checkpoint的日志文件，一般1000触发一次*/
static WT_THREAD_RET __log_server(void* arg)
{
	WT_CONNECTION_IMPL *conn;
	WT_DECL_RET;
	WT_LOG *log;
	WT_SESSION_IMPL *session;
	u_int locked;

	session = (WT_SESSION_IMPL *)arg;
	conn = S2C(session);
	log = conn->log;
	locked = 0;

	while(F_ISSET(conn, WT_CONN_LOG_SERVER_RUN)){
		/*进行预分配日志文件*/
		if (conn->log_prealloc > 0)
			WT_ERR(__log_prealloc_once(session));

		if(FLD_ISSET(conn->log_flags, WT_CONN_LOG_ARCHIVE)){
			/*删除已经checkpoint的日志文件,注意：其实WT整个引擎很少发生对log_archive_lock竞争，所以即使使用spin lock也消耗不大*/
			if(__wt_try_writelock(session, log->log_archive_lock) == 0){
				locked = 1;
				WT_ERR(__log_archive_once(session, 0));
				WT_ERR(	__wt_writeunlock(session, log->log_archive_lock));
				locked = 0;
			}
			else{
				WT_ERR(__wt_verbose(session, WT_VERB_LOG, "log_archive: Blocked due to open log cursor holding archive lock"));
			}
		}
		
		WT_ERR(__wt_cond_wait(session, conn->log_cond, WT_MILLION)); /*1000秒*/
	}
	return WT_THREAD_RET_VALUE;

err:
	__wt_err(session, ret, "log server error");
	if (locked)
		(void)__wt_writeunlock(session, log->log_archive_lock);

	return WT_THREAD_RET_VALUE;
}

/*初始化wiredtiger的日志系统*/
int __wt_logmgr_create(WT_SESSION_IMPL *session, const char *cfg[])
{
	WT_CONNECTION_IMPL *conn;
	WT_LOG *log;
	int run;

	conn = S2C(session);

	/*读取日志文件的配置文件*/
	WT_RET(__logmgr_config(session, cfg, &run));

	if(!run)
		return 0;

	/*设置日志的启动模式*/
	FLD_SET(conn->log_flags, WT_CONN_LOG_ENABLED);

	WT_RET(__wt_calloc_one(session, &conn->log));
	log = conn->log;
	/*初始化各个日志spin lock*/
	WT_RET(__wt_spin_init(session, &log->log_lock, "log"));
	WT_RET(__wt_spin_init(session, &log->log_slot_lock, "log slot"));
	WT_RET(__wt_spin_init(session, &log->log_sync_lock, "log sync"));
	WT_RET(__wt_rwlock_alloc(session, &log->log_archive_lock, "log archive lock"));
	/*设置日志记录数据的对齐长度*/
	if (FLD_ISSET(conn->direct_io, WT_FILE_TYPE_LOG))
		log->allocsize = WT_MAX((uint32_t)conn->buffer_alignment, LOG_ALIGN);
	else
		log->allocsize = LOG_ALIGN;

	/*初始化各种LSN*/
	WT_INIT_LSN(&log->alloc_lsn);
	WT_INIT_LSN(&log->ckpt_lsn);
	WT_INIT_LSN(&log->first_lsn);
	WT_INIT_LSN(&log->sync_lsn);

	WT_ZERO_LSN(&log->sync_dir_lsn);
	WT_INIT_LSN(&log->trunc_lsn);
	WT_INIT_LSN(&log->write_lsn);

	log->fileid = 0;
	WT_RET(__wt_cond_alloc(session, "log sync", 0, &log->log_sync_cond));
	WT_RET(__wt_cond_alloc(session, "log write", 0, &log->log_write_cond));
	WT_RET(__wt_log_open(session));
	WT_RET(__wt_log_slot_init(session));

	return 0;
}

/*启动wiredtiger日志系统*/
int __wt_logmgr_open(WT_SESSION_IMPL *session)
{
	WT_CONNECTION_IMPL *conn;

	conn = S2C(session);

	if(!FLD_ISSET(conn->log_flags, WT_CONN_LOG_ENABLED))
		return 0;

	/*为日志系统分配一个log close session,启动一个log close session*/
	WT_RET(__wt_open_internal_session(conn, "log-close-server", 0, 0, &conn->log_close_session));
	WT_RET(__wt_cond_alloc(conn->log_close_session,"log close server", 0, &conn->log_close_cond));
	WT_RET(__wt_thread_create(conn->log_close_session, &conn->log_close_tid, __log_close_server, conn->log_close_session));
	conn->log_close_tid_set = 1;

	/*创建一个wrlsn的session,并启动一个wrlsn thread*/
	WT_RET(__wt_open_internal_session(conn, "log-wrlsn-server", 0, 0, &conn->log_wrlsn_session));
	WT_RET(__wt_cond_alloc(conn->log_wrlsn_session, "log write lsn server", 0, &conn->log_wrlsn_cond));
	WT_RET(__wt_thread_create(conn->log_wrlsn_session, &conn->log_wrlsn_tid, __log_wrlsn_server, conn->log_wrlsn_session));
	conn->log_wrlsn_tid_set = 1;

	/*如果日志没有配置归档和预分配，则直接返回*/
	if(!FLD_ISSET(conn->log_flags, WT_CONN_LOG_ARCHIVE | WT_CONN_LOG_PREALLOC))
		return 0;

	/*如果已经建立log_session,触发一个log_cond信号，让log_server执行一次*/
	if (conn->log_session != NULL) {
		WT_ASSERT(session, conn->log_cond != NULL);
		WT_ASSERT(session, conn->log_tid_set != 0);
		WT_RET(__wt_cond_signal(session, conn->log_cond));
	} else {
		/* The log server gets its own session. */
		WT_RET(__wt_open_internal_session(conn, "log-server", 0, 0, &conn->log_session));
		WT_RET(__wt_cond_alloc(conn->log_session, "log server", 0, &conn->log_cond));

		/*启动一个__log_server线程，用于预分配日志文件和日志归档*/
		WT_RET(__wt_thread_create(conn->log_session, &conn->log_tid, __log_server, conn->log_session));
		conn->log_tid_set = 1;
	}

	return 0;
}

/*关闭日志系统，并进行其进行内存释放*/
int __wt_logmgr_destroy(WT_SESSION_IMPL *session)
{
	WT_CONNECTION_IMPL *conn;
	WT_DECL_RET;
	WT_SESSION *wt_session;

	conn = S2C(session);
	/*日志模块没启动*/
	if(!FLD_ISSET(conn->log_flags, WT_CONN_LOG_ENABLED)){
		__wt_free(session, conn->log_path);
		return 0;
	}

	/*等待log_server线程的退出*/
	if (conn->log_tid_set) {
		WT_TRET(__wt_cond_signal(session, conn->log_cond));
		WT_TRET(__wt_thread_join(session, conn->log_tid));
		conn->log_tid_set = 0;
	}
	WT_TRET(__wt_cond_destroy(session, &conn->log_cond));

	if (conn->log_close_tid_set) {
		WT_TRET(__wt_cond_signal(session, conn->log_close_cond));
		WT_TRET(__wt_thread_join(session, conn->log_close_tid));
		conn->log_close_tid_set = 0;
	}

	WT_TRET(__wt_cond_destroy(session, &conn->log_close_cond));
	if (conn->log_close_session != NULL) {
		wt_session = &conn->log_close_session->iface;
		WT_TRET(wt_session->close(wt_session, NULL));
		conn->log_close_session = NULL;
	}

	if (conn->log_wrlsn_tid_set) {
		WT_TRET(__wt_cond_signal(session, conn->log_wrlsn_cond));
		WT_TRET(__wt_thread_join(session, conn->log_wrlsn_tid));
		conn->log_wrlsn_tid_set = 0;
	}
	WT_TRET(__wt_cond_destroy(session, &conn->log_wrlsn_cond));

	if (conn->log_wrlsn_session != NULL) {
		wt_session = &conn->log_wrlsn_session->iface;
		WT_TRET(wt_session->close(wt_session, NULL));
		conn->log_wrlsn_session = NULL;
	}
	WT_TRET(__wt_log_close(session));

	if (conn->log_session != NULL) {
		wt_session = &conn->log_session->iface;
		WT_TRET(wt_session->close(wt_session, NULL));
		conn->log_session = NULL;
	}

	/*释放log对象*/
	WT_TRET(__wt_log_slot_destroy(session));
	WT_TRET(__wt_cond_destroy(session, &conn->log->log_sync_cond));
	WT_TRET(__wt_cond_destroy(session, &conn->log->log_write_cond));
	WT_TRET(__wt_rwlock_destroy(session, &conn->log->log_archive_lock));
	__wt_spin_destroy(session, &conn->log->log_lock);
	__wt_spin_destroy(session, &conn->log->log_slot_lock);
	__wt_spin_destroy(session, &conn->log->log_sync_lock);
	__wt_free(session, conn->log_path);
	__wt_free(session, conn->log);

	return ret;
}





