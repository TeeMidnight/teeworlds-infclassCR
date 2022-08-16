#ifdef CONF_SQL
/* SQL Class by Sushi */

#include <engine/shared/protocol.h>

#include <mysql_connection.h>
	
#include <cppconn/driver.h>
#include <cppconn/exception.h>
#include <cppconn/resultset.h>
#include <cppconn/statement.h>

class CSQL
{
public:
	CSQL(class CGameContext *pGameServer);

	sql::Driver *driver;
	sql::Connection *connection;
	sql::Statement *statement;
	sql::ResultSet *results;
	
	// copy of config vars
	const char* Database;
	const char* prefix;
	const char* user;
	const char* pass;
	const char* ip;
	int port;
	
	bool connect();
	void disconnect();
	
	void create_tables();
	void create_account(const char* name, const char* pass, int client_id);
	void delete_account(const char* name);
	void delete_account(int client_id);
	void change_password(int client_id, const char* new_pass);

	void login(const char* name, const char* pass, int client_id);
	void update(int client_id);
	void update_all();
	void UpdateCK(int ClientID, const char* CK, const char* Num);
	void SyncAccountData(int ClientID);

/*	static void change_password_thread(void *user);
	static void login_thread(void *user);
	static void update_thread(void *user);
	static void create_account_thread(void *user);*/
};

struct CSqlData
{
	CSQL *m_SqlData;
	int UserID[MAX_CLIENTS];
	char name[32];
	char pass[32];
	const char* m_Resource;
	const char* m_Num;
	int m_Sword[MAX_CLIENTS];
	int m_Pickaxe[MAX_CLIENTS];
	int m_Axe[MAX_CLIENTS];
	int m_Wave[MAX_CLIENTS];
	int m_ClientID;
};

struct CAccountData
{
	int UserID[MAX_CLIENTS];
	
	bool m_LoggedIn[MAX_CLIENTS];
};
#endif