#ifdef CONF_SQL
/* SQL class 0.5 by Sushi */
/* SQL class 0.6 by FFS   */
/* infclassCR version by ErrorDreemurr */
#include <game/server/gamecontext.h>

#include <engine/shared/config.h>

static LOCK SQLLock = 0;
class CGameContext *m_pGameServer;
CGameContext *GameServer() { return m_pGameServer; }

CSQL::CSQL(class CGameContext *pGameServer)
{
	if(SQLLock == 0)
		SQLLock = lock_create();

	m_pGameServer = pGameServer;
		
	// set Database info
	Database = g_Config.m_SvSqlDatabase;
	prefix = g_Config.m_SvSqlPrefix;
	user = g_Config.m_SvSqlUser;
	pass = g_Config.m_SvSqlPassword;
	ip = g_Config.m_SvSqlIp;
	port = g_Config.m_SvSqlPort;
}

bool CSQL::connect()
{
	try 
	{
		driver = 0;
		connection = 0;
		// Create connection

		sql::ConnectOptionsMap connection_properties;
		connection_properties["hostName"]      = sql::SQLString(ip);
		connection_properties["port"]          = port;
		connection_properties["userName"]      = sql::SQLString(user);
		connection_properties["password"]      = sql::SQLString(pass);
		connection_properties["OPT_CONNECT_TIMEOUT"] = 10;
		connection_properties["OPT_READ_TIMEOUT"] = 10;
		connection_properties["OPT_WRITE_TIMEOUT"] = 20;
		connection_properties["OPT_RECONNECT"] = true;
		
		driver = get_driver_instance();
		connection = driver->connect(connection_properties);
		
		// Create Statement
		statement = connection->createStatement();
		
		char aBuf[128];
		// Create Database if not exists
		str_format(aBuf, sizeof(aBuf), "CREATE Database IF NOT EXISTS %s", Database);
		statement->execute(aBuf);
		
		// Connect to specific Database
		connection->setSchema(Database);
		return true;
	} 
	catch (sql::SQLException &e)
	{
		dbg_msg("SQL", "ERROR: SQL connection failed (%s)", e.what());
		return false;
	}
}

void CSQL::disconnect()
{
	try
	{
		delete connection;
		dbg_msg("SQL", "SQL connection disconnected");
	}
	catch (sql::SQLException &e)
	{
		dbg_msg("SQL", "ERROR: No SQL connection (%s)", e.what());
	}
}

// create tables... should be done only once
void CSQL::create_tables()
{
	// create connection
	if(connect())
	{
		try
		{
			// create tables
			char buf[2048];
			str_format(buf, sizeof(buf), 
			"CREATE TABLE IF NOT EXISTS %s_Account "
			"(UserID INT AUTO_INCREMENT PRIMARY KEY, "
			"Username VARCHAR(31) NOT NULL, "
			"Password VARCHAR(32) NOT NULL, "
			"Score BIGINT DEFAULT 0, "
			, prefix);
			statement->execute(buf);

			str_format(buf, sizeof(buf), 
			"CREATE TABLE IF NOT EXISTS %s_Round "
			"RoundHash VARCHAR(32) NOT NULL, "
			"RoundWinner BOOLEN DEFAULT 0, "
			"RoundNumHuman BIGINT DEFAULT 0, "
			, prefix);

			statement->execute(buf);
			dbg_msg("SQL", "Tables were created successfully");

			// delete statement
			delete statement;
		}
		catch (sql::SQLException &e)
		{
			dbg_msg("SQL", "ERROR: Tables were NOT created (%s)", e.what());
		}
		
		// disconnect from Database
		disconnect();
	}	
}

// create Account
static void create_account_thread(void *user)
{
	lock_wait(SQLLock);
	
	CSqlData *Data = (CSqlData *)user;
	
	if(GameServer()->m_apPlayers[Data->m_ClientID])
	{
		// Connect to Database
		if(Data->m_SqlData->connect())
		{
			try
			{
				// check if allready exists
				char buf[512];
				str_format(buf, sizeof(buf), "SELECT * FROM %s_Account WHERE Username='%s';", Data->m_SqlData->prefix, Data->name);
				Data->m_SqlData->results = Data->m_SqlData->statement->executeQuery(buf);
				if(Data->m_SqlData->results->next())
				{
					// Account found
					dbg_msg("SQL", "Account '%s' already exists", Data->name);
					
					GameServer()->SendChatTarget(Data->m_ClientID, "This acoount already exists!");
				}
				else
				{
					// create Account \o/
					str_format(buf, sizeof(buf), "INSERT INTO %s_Account(Username, Password) VALUES ('%s', '%s');", 
					Data->m_SqlData->prefix, 
					Data->name, Data->pass);
					
					Data->m_SqlData->statement->execute(buf);
					dbg_msg("SQL", "Account '%s' was successfully created", Data->name);
					
					GameServer()->SendChatTarget(Data->m_ClientID, "Acoount was created successfully.");
					Data->m_SqlData->login(Data->name, Data->pass, Data->m_ClientID);
				}
				
				// delete statement
				delete Data->m_SqlData->statement;
				delete Data->m_SqlData->results;
			}
			catch (sql::SQLException &e)
			{
				dbg_msg("SQL", "ERROR: Could not create Account (%s)", e.what());
			}
			
			// disconnect from Database
			Data->m_SqlData->disconnect();
		}
	}
	
	delete Data;
	
	lock_unlock(SQLLock);
}

void CSQL::create_account(const char* name, const char* pass, int m_ClientID)
{
	CSqlData *tmp = new CSqlData();
	str_copy(tmp->name, name, sizeof(tmp->name));
	str_copy(tmp->pass, pass, sizeof(tmp->pass));
	tmp->m_ClientID = m_ClientID;
	tmp->m_SqlData = this;
	
	void *register_thread = thread_init(create_account_thread, tmp);
#if defined(CONF_FAMILY_UNIX)
	pthread_detach((pthread_t)register_thread);
#endif
}

// change password
static void change_password_thread(void *user)
{
	lock_wait(SQLLock);
	
	CSqlData *Data = (CSqlData *)user;
	
	// Connect to Database
	if(Data->m_SqlData->connect())
	{
		try
		{
			// Connect to Database
			Data->m_SqlData->connect();
			
			// check if Account exists
			char buf[512];
			str_format(buf, sizeof(buf), "SELECT * FROM %s_Account WHERE Username='%s';", Data->m_SqlData->prefix, Data->name[Data->m_ClientID]);
			Data->m_SqlData->results = Data->m_SqlData->statement->executeQuery(buf);
			if(Data->m_SqlData->results->next())
			{
				// update Account data
				str_format(buf, sizeof(buf), "UPDATE %s_Account SET Password='%s' WHERE UserID=%d", Data->m_SqlData->prefix, Data->pass, Data->UserID[Data->m_ClientID]);
				Data->m_SqlData->statement->execute(buf);
				
				// get Account name from Database
				str_format(buf, sizeof(buf), "SELECT Username FROM %s_Account WHERE UserID=%d;", Data->m_SqlData->prefix, Data->UserID[Data->m_ClientID]);
				
				// create results
				Data->m_SqlData->results = Data->m_SqlData->statement->executeQuery(buf);

				// jump to result
				Data->m_SqlData->results->next();
				
				// finally the name is there \o/
				char acc_name[32];
				str_copy(acc_name, Data->m_SqlData->results->getString("Username").c_str(), sizeof(acc_name));	
				dbg_msg("SQL", "Account '%s' changed password.", acc_name);
				
				// Success
				str_format(buf, sizeof(buf), "Successfully changed your password to '%s'.", Data->pass);
				GameServer()->SendChatTarget(Data->m_ClientID, buf);
			}
			else
				dbg_msg("SQL", "Account seems to be deleted");
			
			// delete statement and results
			delete Data->m_SqlData->statement;
			delete Data->m_SqlData->results;
		}
		catch (sql::SQLException &e)
		{
			dbg_msg("SQL", "ERROR: Could not update Account (Why: %s) (ClientID: %d, UserID: %d)", e.what(), Data->m_ClientID, Data->UserID);
		}
		
		// disconnect from Database
		Data->m_SqlData->disconnect();
	}
	
	delete Data;
	
	lock_unlock(SQLLock);
}

void CSQL::change_password(int m_ClientID, const char* new_pass)
{
	CSqlData *tmp = new CSqlData();
	tmp->m_ClientID = m_ClientID;
	tmp->UserID[m_ClientID] = GameServer()->m_apPlayers[m_ClientID]->m_AccData.m_UserID;
	str_copy(tmp->pass, new_pass, sizeof(tmp->pass));
	tmp->m_SqlData = this;
	
	void *change_pw_thread = thread_init(change_password_thread, tmp);
#if defined(CONF_FAMILY_UNIX)
	pthread_detach((pthread_t)change_pw_thread);
#endif
}

// login stuff
static void login_thread(void *user)
{
	lock_wait(SQLLock);
	
	CSqlData *Data = (CSqlData *)user;

	if(GameServer()->m_apPlayers[Data->m_ClientID] && !GameServer()->m_apPlayers[Data->m_ClientID]->LoggedIn)
	{
		// Connect to Database
		if(Data->m_SqlData->connect())
		{
			try
			{		
				// check if Account exists
				char buf[1024];
				str_format(buf, sizeof(buf), "SELECT * FROM %s_Account WHERE Username='%s';", Data->m_SqlData->prefix, Data->name);
				Data->m_SqlData->results = Data->m_SqlData->statement->executeQuery(buf);
				if(Data->m_SqlData->results->next())
				{
					// check for right pw and get data
					str_format(buf, sizeof(buf), "SELECT * "
					"FROM %s_Account WHERE Username='%s' AND Password='%s';", Data->m_SqlData->prefix, Data->name, Data->pass);
					
					// create results
					Data->m_SqlData->results = Data->m_SqlData->statement->executeQuery(buf);
					
					// if match jump to it
					if(Data->m_SqlData->results->next())
					{
						// never use player directly!
						// finally save the result to AccountData() \o/

						// check if Account allready is logged in
						for(int i = 0; i < MAX_CLIENTS; i++)
						{
							if(!GameServer()->m_apPlayers[i])
								continue;

							if(GameServer()->m_apPlayers[i]->m_AccData.m_UserID == Data->m_SqlData->results->getInt("UserID"))
							{								
								GameServer()->SendChatTarget(Data->m_ClientID, _("This Account is already logged in."));
								
								// delete statement and results
								delete Data->m_SqlData->statement;
								delete Data->m_SqlData->results;
								
								// disconnect from Database
								Data->m_SqlData->disconnect();
								
								// delete Data
								delete Data;
	
								// release lock
								lock_unlock(SQLLock);
								
								return;
							}
						}
						// login should be the last thing
						GameServer()->m_apPlayers[Data->m_ClientID]->LoggedIn = true;
						dbg_msg("SQL", "Account '%s' logged in sucessfully", Data->name);
						
						GameServer()->SendChatTarget(Data->m_ClientID, _("You are now logged in."));}
					else
					{
						// wrong password
						dbg_msg("SQL", "Account '%s' is not logged in due to wrong password", Data->name);
						
						GameServer()->SendChatTarget(Data->m_ClientID, _("The password you entered is wrong."));
					}
				}
				else
				{
					// no Account
					dbg_msg("SQL", "Account '%s' does not exists", Data->name);
					
					GameServer()->SendChatTarget(Data->m_ClientID, _("This Account does not exists."));
					GameServer()->SendChatTarget(Data->m_ClientID, _("Please register first. (/register <user> <pass>)"));
				}
				
				// delete statement and results
				delete Data->m_SqlData->statement;
				delete Data->m_SqlData->results;
			}
			catch (sql::SQLException &e)
			{
				dbg_msg("SQL", "ERROR: Could not login Account (%s)", e.what());
			}
			
			// disconnect from Database
			Data->m_SqlData->disconnect();
		}
	}
	
	delete Data;
	
	lock_unlock(SQLLock);
}

void CSQL::login(const char* name, const char* pass, int m_ClientID)
{
	CSqlData *tmp = new CSqlData();
	str_copy(tmp->name, name, sizeof(tmp->name));
	str_copy(tmp->pass, pass, sizeof(tmp->pass));
	tmp->m_ClientID = m_ClientID;
	tmp->m_SqlData = this;
	
	void *login_account_thread = thread_init(login_thread, tmp);
#if defined(CONF_FAMILY_UNIX)
	pthread_detach((pthread_t)login_account_thread);
#endif
}

static void SyncThread(void *user)
{
	lock_wait(SQLLock);
	
	CSqlData *Data = (CSqlData *)user;

	if(GameServer()->m_apPlayers[Data->m_ClientID] && GameServer()->m_apPlayers[Data->m_ClientID]->LoggedIn)
	{
		// Connect to Database
		if(Data->m_SqlData->connect())
		{
			try
			{		
				char buf[1024];
				str_format(buf, sizeof(buf), "SELECT * FROM %s_Account WHERE UserID=%d;", Data->m_SqlData->prefix, Data->UserID[Data->m_ClientID]);
				Data->m_SqlData->results = Data->m_SqlData->statement->executeQuery(buf);
				if(Data->m_SqlData->results->next())
				{
					str_format(buf, sizeof(buf), "SELECT * "
					"FROM %s_Account WHERE UserID=%d;", Data->m_SqlData->prefix, Data->UserID[Data->m_ClientID]);
					
					// create results
					Data->m_SqlData->results = Data->m_SqlData->statement->executeQuery(buf);
				}
				else
					dbg_msg("SQL", "Account '%s' does not exists", Data->name);
				
				// delete statement and results
				delete Data->m_SqlData->statement;
				delete Data->m_SqlData->results;
			}
			catch (sql::SQLException &e)
			{
				dbg_msg("SQL", "ERROR: Could not login Account (%s)", e.what());
			}
			
			// disconnect from Database
			Data->m_SqlData->disconnect();
		}
	}
	
	delete Data;
	
	lock_unlock(SQLLock);
}

// update stuff
static void update_thread(void *user)
{
	lock_wait(SQLLock);
	
	CSqlData *Data = (CSqlData *)user;

	// Connect to Database
	if(Data->m_SqlData->connect())
	{
		try
		{
			// check if Account exists
			char buf[1024];
			str_format(buf, sizeof(buf), "SELECT * FROM %s_Account WHERE UserID=%d;", Data->m_SqlData->prefix, Data->UserID[Data->m_ClientID]);
			Data->m_SqlData->results = Data->m_SqlData->statement->executeQuery(buf);
			if(Data->m_SqlData->results->next())
			{
				// update Account data
				CPlayer *p = GameServer()->m_apPlayers[Data->m_ClientID];
				if(!p)
				{
					lock_unlock(SQLLock);
					return;
				}
				str_format(buf, sizeof(buf), "UPDATE %s_Account SET " \
				"WHERE UserID=%d", \
				Data->m_SqlData->prefix,
				Data->UserID[Data->m_ClientID] \
				);
				Data->m_SqlData->statement->execute(buf);
				// get Account name from database
				str_format(buf, sizeof(buf), "SELECT Username FROM %s_Account WHERE UserID=%d;", Data->m_SqlData->prefix, Data->UserID[Data->m_ClientID]);
				// create results
				Data->m_SqlData->results = Data->m_SqlData->statement->executeQuery(buf);
				// jump to result
				Data->m_SqlData->results->next();
				
				// finally the nae is there \o/
				char acc_name[32];
				str_copy(acc_name, Data->m_SqlData->results->getString("Username").c_str(), sizeof(acc_name));	
				dbg_msg("SQL", "Account '%s' was saved successfully", acc_name);
			}
			else
				dbg_msg("SQL", "Account seems to be deleted");
			
			// delete statement and results
			delete Data->m_SqlData->statement;
			delete Data->m_SqlData->results;
		}
		catch (sql::SQLException &e)
		{
			dbg_msg("SQL", "ERROR: Could not update Account (Why: %s) (ClientID: %d, UserID: %d)", e.what(), Data->m_ClientID, Data->UserID[Data->m_ClientID]);
		}
		
		// disconnect from Database
		Data->m_SqlData->disconnect();
	}
	
	delete Data;
	
	lock_unlock(SQLLock);
}

void CSQL::update(int m_ClientID)
{
	CSqlData *tmp = new CSqlData();
	tmp->m_ClientID = m_ClientID;
	tmp->UserID[m_ClientID] = GameServer()->m_apPlayers[m_ClientID]->m_AccData.m_UserID;

	tmp->m_SqlData = this;
	
	void *update_account_thread = thread_init(update_thread, tmp);
#if defined(CONF_FAMILY_UNIX)
	pthread_detach((pthread_t)update_account_thread);
#endif
}

static void UpdateResourceThread(void *user)
{
	lock_wait(SQLLock);
	
	CSqlData *Data = (CSqlData *)user;

	// Connect to Database
	if(Data->m_SqlData->connect())
	{
		try
		{
			// check if Account exists
			char buf[1024];
			str_format(buf, sizeof(buf), "SELECT * FROM %s_Account WHERE UserID=%d;", Data->m_SqlData->prefix, Data->UserID[Data->m_ClientID]);
			Data->m_SqlData->results = Data->m_SqlData->statement->executeQuery(buf);
			if(Data->m_SqlData->results->next())
			{
				// update Account data
				CPlayer *p = GameServer()->m_apPlayers[Data->m_ClientID];
				if(!p)
				{
					lock_unlock(SQLLock);
					return;
				}
				str_format(buf, sizeof(buf), "UPDATE %s_Account SET %s=%s%s WHERE UserID=%d;", Data->m_SqlData->prefix, Data->m_Resource, Data->m_Resource, Data->m_Num, Data->UserID[Data->m_ClientID]);
				Data->m_SqlData->statement->execute(buf);
			}
			else
				dbg_msg("SQL", "Account seems to be deleted");
			
			// delete statement and results
			delete Data->m_SqlData->statement;
			delete Data->m_SqlData->results;
		}
		catch (sql::SQLException &e)
		{
			dbg_msg("SQL", "ERROR: Could not update Account (Why: %s) (ClientID: %d, UserID: %d)", e.what(), Data->m_ClientID, Data->UserID[Data->m_ClientID]);
		}

		Data->m_SqlData->disconnect();
	}
	lock_unlock(SQLLock);
	Data->m_SqlData->SyncAccountData(Data->m_ClientID);
}
// update all
void CSQL::update_all()
{
	lock_wait(SQLLock);
	
	// Connect to Database
	if(connect())
	{
		try
		{
			char buf[512];
			char acc_name[32];
			for(int i = 0; i < MAX_CLIENTS; i++)
			{
				if(!GameServer()->m_apPlayers[i])
					continue;
				
				if(!GameServer()->m_apPlayers[i]->LoggedIn)
					continue;
				
				// check if Account exists
				str_format(buf, sizeof(buf), "SELECT * FROM %s_Account WHERE UserID=%d;", prefix, GameServer()->m_apPlayers[i]->m_AccData.m_UserID);
				results = statement->executeQuery(buf);
				if(results->next())
				{
					CPlayer *p = GameServer()->m_apPlayers[i];
					str_format(buf, sizeof(buf), "UPDATE %s_Account SET "
					"WHERE UserID=%d;",
					prefix,p->m_AccData.m_UserID
					);

					// create results
					results = statement->executeQuery(buf);

					// jump to result
					results->next();
					
					// finally the name is there \o/	
					str_copy(acc_name, results->getString("Username").c_str(), sizeof(acc_name));
					dbg_msg("SQL", "Account '%s' was saved successfully", acc_name);
				}
				else
					dbg_msg("SQL", "Account seems to be deleted");
				
				// delete results
				delete results;
			}
			
			// delete statement
			delete statement;
		}
		catch (sql::SQLException &e)
		{
			dbg_msg("SQL", "ERROR: Could not update Account (Why: %s)");
		}
		
		// disconnect from Database
		disconnect();
	}

	lock_unlock(SQLLock);
}
	
void CSQL::SyncAccountData(int ClientID)
{
	CSqlData *tmp = new CSqlData();
	tmp->m_ClientID = ClientID;
	tmp->UserID[ClientID] = GameServer()->m_apPlayers[ClientID]->m_AccData.m_UserID;
	tmp->m_SqlData = this;
	
	void *Sync_Thread = thread_init(SyncThread, tmp);
#if defined(CONF_FAMILY_UNIX)
	pthread_detach((pthread_t)Sync_Thread);
#endif
}

void CSQL::UpdateCK(int ClientID, const char* CK, const char* Num)
{
	CSqlData *tmp = new CSqlData();
	tmp->m_ClientID = ClientID;
	tmp->UserID[ClientID] = GameServer()->m_apPlayers[ClientID]->m_AccData.m_UserID;
	tmp->m_Resource = CK;
	tmp->m_Num = Num;

	tmp->m_SqlData = this;
	
	void *update_ck_thread = thread_init(UpdateResourceThread, tmp);
#if defined(CONF_FAMILY_UNIX)
	pthread_detach((pthread_t)update_ck_thread);
#endif
}
#endif