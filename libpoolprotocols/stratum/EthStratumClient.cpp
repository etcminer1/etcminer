#include "EthStratumClient.h"
#include <libdevcore/Log.h>
#include <libethash/endian.h>
#include <etcminer-buildinfo.h>

#ifdef _WIN32
#include <wincrypt.h>
#endif

using boost::asio::ip::tcp;

static uint64_t bswap(uint64_t val)
{
    val = ((val << 8) & 0xFF00FF00FF00FF00ULL) | ((val >> 8) & 0x00FF00FF00FF00FFULL);
    val = ((val << 16) & 0xFFFF0000FFFF0000ULL) | ((val >> 16) & 0x0000FFFF0000FFFFULL);
    return (val << 32) | (val >> 32);
}


static void diffToTarget(uint32_t *target, double diff)
{
	uint32_t target2[8];
	uint64_t m;
	int k;

	for (k = 6; k > 0 && diff > 1.0; k--)
		diff /= 4294967296.0;
	m = (uint64_t)(4294901760.0 / diff);
	if (m == 0 && k == 6)
		memset(target2, 0xff, 32);
	else {
		memset(target2, 0, 32);
		target2[k] = (uint32_t)m;
		target2[k + 1] = (uint32_t)(m >> 32);
	}

	for (int i = 0; i < 32; i++)
		((uint8_t*)target)[31 - i] = ((uint8_t*)target2)[i];
}


EthStratumClient::EthStratumClient(int const & worktimeout, string const & email, bool const & submitHashrate) : PoolClient(),
	m_socket(nullptr),
	m_conntimer(m_io_service),
	m_worktimer(m_io_service),
	m_responsetimer(m_io_service),
	m_resolver(m_io_service)
{

	m_worktimeout = worktimeout;
	m_email = email;

	m_submit_hashrate = submitHashrate;
	if (m_submit_hashrate)
		m_submit_hashrate_id = h256::random().hex();
}

EthStratumClient::~EthStratumClient()
{
	m_io_service.stop();
	m_serviceThread.join();
}

void EthStratumClient::connect()
{

	m_connected.store(false, std::memory_order_relaxed);
	m_subscribed.store(false, std::memory_order_relaxed);
	m_authorized.store(false, std::memory_order_relaxed);


	// Prepare Socket

	if (m_conn.SecLevel() != SecureLevel::NONE) {

		boost::asio::ssl::context::method method = boost::asio::ssl::context::tls_client;
		if (m_conn.SecLevel() == SecureLevel::TLS12)
			method = boost::asio::ssl::context::tlsv12;

		boost::asio::ssl::context ctx(method);
		m_securesocket = std::make_shared<boost::asio::ssl::stream<boost::asio::ip::tcp::socket> >(m_io_service, ctx);
		m_socket = &m_securesocket->next_layer();

		if (m_conn.SecLevel() != SecureLevel::ALLOW_SELFSIGNED) {
			m_securesocket->set_verify_mode(boost::asio::ssl::verify_peer);

#ifdef _WIN32
			HCERTSTORE hStore = CertOpenSystemStore(0, "ROOT");
			if (hStore == NULL) {
				return;
			}

			X509_STORE *store = X509_STORE_new();
			PCCERT_CONTEXT pContext = NULL;
			while ((pContext = CertEnumCertificatesInStore(hStore, pContext)) != NULL) {
				X509 *x509 = d2i_X509(NULL,
					(const unsigned char **)&pContext->pbCertEncoded,
					pContext->cbCertEncoded);
				if (x509 != NULL) {
					X509_STORE_add_cert(store, x509);
					X509_free(x509);
				}
			}

			CertFreeCertificateContext(pContext);
			CertCloseStore(hStore, 0);

			SSL_CTX_set_cert_store(ctx.native_handle(), store);
#else
			char *certPath = getenv("SSL_CERT_FILE");
			try {
				ctx.load_verify_file(certPath ? certPath : "/etc/ssl/certs/ca-certificates.crt");
			}
			catch (...) {
				cwarn << "Failed to load ca certificates. Either the file '/etc/ssl/certs/ca-certificates.crt' does not exist";
				cwarn << "or the environment variable SSL_CERT_FILE is set to an invalid or inaccessable file.";
				cwarn << "It is possible that certificate verification can fail.";
			}
#endif
		}
	}
	else {
	  m_nonsecuresocket = std::make_shared<boost::asio::ip::tcp::socket>(m_io_service);
	  m_socket = m_nonsecuresocket.get();
	}

	// Activate keep alive to detect disconnects
	unsigned int keepAlive = 10000;

#if defined _WIN32 || defined WIN32 || defined OS_WIN64 || defined _WIN64 || defined WIN64 || defined WINNT
	int32_t timeout = keepAlive;
	setsockopt(m_socket->native_handle(), SOL_SOCKET, SO_RCVTIMEO, (const char*)&timeout, sizeof(timeout));
	setsockopt(m_socket->native_handle(), SOL_SOCKET, SO_SNDTIMEO, (const char*)&timeout, sizeof(timeout));
#else
	struct timeval tv;
	tv.tv_sec = keepAlive / 1000;
	tv.tv_usec = keepAlive % 1000;
	setsockopt(m_socket->native_handle(), SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
	setsockopt(m_socket->native_handle(), SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
#endif

	// Begin resolve and connect
	tcp::resolver::query q(m_conn.Host(), toString(m_conn.Port()));
	m_resolver.async_resolve(q,
		boost::bind(&EthStratumClient::resolve_handler,
			this, boost::asio::placeholders::error,
			boost::asio::placeholders::iterator));


	// IMPORTANT !!
	if (m_serviceThread.joinable())
	{
		// If the service thread have been created try to reset the service.
		m_io_service.reset();
	}
	else
	{
		// Otherwise, if the first time here, create new thread.
		m_serviceThread = std::thread{ boost::bind(&boost::asio::io_service::run, &m_io_service) };
	}



}

#define BOOST_ASIO_ENABLE_CANCELIO 

void EthStratumClient::disconnect()
{
	// Prevent unnecessary recursion
	if (m_disconnecting.load(std::memory_order::memory_order_relaxed)) {
		return;
	}
	else {
		m_disconnecting.store(true, std::memory_order::memory_order_relaxed);
	}

	m_conntimer.cancel();
	m_worktimer.cancel();
	m_responsetimer.cancel();
	m_response_pending = false;

	if (m_socket && m_socket->is_open()) { 

		try {
		
			boost::system::error_code sec;

			if (m_conn.SecLevel() != SecureLevel::NONE) {
				m_securesocket->shutdown(sec);
			}
			else {
				m_nonsecuresocket->shutdown(boost::asio::ip::tcp::socket::shutdown_both, sec);
			}

			m_socket->close();
			m_io_service.stop();
		}
		catch (std::exception const& _e) {
			cwarn << "Error while disconnecting:" << _e.what();
		}

		m_securesocket = nullptr;
		m_nonsecuresocket = nullptr;
		m_socket = nullptr;
	}


	m_subscribed.store(false, std::memory_order_relaxed);
	m_authorized.store(false, std::memory_order_relaxed);

	// Release locking flag and set connection status
	m_connected.store(false, std::memory_order_relaxed);
	m_disconnecting.store(false, std::memory_order::memory_order_relaxed);

	// Trigger handlers
	if (m_onDisconnected) { m_onDisconnected();	}

}

void EthStratumClient::resolve_handler(const boost::system::error_code& ec, tcp::resolver::iterator i)
{
	dev::setThreadName("stratum");
	if (!ec)
	{

		// Start Connection Process and set timeout timer
		start_connect(i);
		m_conntimer.async_wait(boost::bind(&EthStratumClient::check_connect_timeout, this, boost::asio::placeholders::error));

	}
	else
	{
		cwarn << "Could not resolve host " << m_conn.Host() << ", " << ec.message();
		disconnect();
	}
}

void EthStratumClient::reset_work_timeout()
{
	m_worktimer.cancel();
	m_worktimer.expires_from_now(boost::posix_time::seconds(m_worktimeout));
	m_worktimer.async_wait(boost::bind(&EthStratumClient::work_timeout_handler, this, boost::asio::placeholders::error));
}

void EthStratumClient::start_connect(tcp::resolver::iterator endpoint_iter)
{
	if (endpoint_iter != tcp::resolver::iterator()) {

		cnote << ("Trying " + toString(endpoint_iter->endpoint()) + " ...");
		
		m_conntimer.expires_from_now(boost::posix_time::seconds(m_conntimeout));

		// Start connecting async
		m_socket->async_connect(endpoint_iter->endpoint(), 
			boost::bind(&EthStratumClient::connect_handler, this, _1, endpoint_iter));

	}
	else {

		cwarn << "No more addresses to try !";
		disconnect();

	}
}

void EthStratumClient::check_connect_timeout(const boost::system::error_code& ec)
{
	(void)ec;

	// Check whether the deadline has passed. We compare the deadline against
	// the current time since a new asynchronous operation may have moved the
	// deadline before this actor had a chance to run.

	if (!isConnected()) {

		if (m_conntimer.expires_at() <= boost::asio::deadline_timer::traits_type::now())
		{
			// The deadline has passed. The socket is closed so that any outstanding
			// asynchronous operations are cancelled.
			m_socket->close();

			// There is no longer an active deadline. The expiry is set to positive
			// infinity so that the actor takes no action until a new deadline is set.
			m_conntimer.expires_at(boost::posix_time::pos_infin);
		}
		// Put the actor back to sleep.
		m_conntimer.async_wait(boost::bind(&EthStratumClient::check_connect_timeout, this, boost::asio::placeholders::error));
	}

}


void EthStratumClient::connect_handler(const boost::system::error_code& ec, tcp::resolver::iterator i)
{
	
	dev::setThreadName("stratum");

	// Timeout has run before
	if (!m_socket->is_open()) {

		cwarn << ("Error  " + toString((i)->endpoint()) + " [Timeout]");

		// Try the next available endpoint.
		start_connect(++i);

	} else if (ec) {

		cwarn << ("Error  " + toString((i)->endpoint()) + " [" + ec.message() + "]");
		
		// We need to close the socket used in the previous connection attempt
		// before starting a new one.
		// In case of error, in fact, boost does not close the socket
		m_socket->close();

		// Try the next available endpoint.
		start_connect(++i);

	}
	else {

		// Immediately set connected flag to prevent 
		// occurrence of subsequents timeouts (if any)
		m_connected.store(true, std::memory_order_relaxed);
		m_conntimer.cancel();

		m_endpoint = (i)->endpoint();

		if (m_conn.SecLevel() != SecureLevel::NONE) {

			boost::system::error_code hec;
			m_securesocket->handshake(boost::asio::ssl::stream_base::client, hec);

			if (hec) {
				cwarn << "SSL/TLS Handshake failed: " << hec.message();
				if (hec.value() == 337047686) { // certificate verification failed
					cwarn << "This can have multiple reasons:";
					cwarn << "* Root certs are either not installed or not found";
					cwarn << "* Pool uses a self-signed certificate";
					cwarn << "Possible fixes:";
					cwarn << "* Make sure the file '/etc/ssl/certs/ca-certificates.crt' exists and is accessible";
					cwarn << "* Export the correct path via 'export SSL_CERT_FILE=/etc/ssl/certs/ca-certificates.crt' to the correct file";
					cwarn << "  On most systems you can install the 'ca-certificates' package";
					cwarn << "  You can also get the latest file here: https://curl.haxx.se/docs/caextract.html";
					cwarn << "* Disable certificate verification all-together via command-line option.";
				}

				// Do not trigger a full disconnection but, instead, let the loop
				// continue with another IP (if any). 
				// Disconnection is triggered on no more IP available
				m_connected.store(false, std::memory_order_relaxed);
				m_socket->close();
				start_connect(++i);
				return;
			}
		}

		// Trigger event handlers and begin counting for the next job
		if (m_onConnected) { m_onConnected(); }
		reset_work_timeout();

		string user;
		size_t p;

		Json::Value jReq;
		jReq["id"] = unsigned(1);
		jReq["method"] = "mining.subscribe";
		jReq["params"] = Json::Value(Json::arrayValue);

		m_worker.clear();
		p = m_conn.User().find_first_of(".");
		if (p != string::npos) {
			user = m_conn.User().substr(0, p);

			// There should be at least one char after dot
			// returned p is zero based
			if (p < (m_conn.User().length() -1))
				m_worker = m_conn.User().substr(++p);
		}
		else
			user = m_conn.User();

		switch (m_conn.Version()) {

			case EthStratumClient::STRATUM:

				jReq["jsonrpc"] = "2.0";

				break;

			case EthStratumClient::ETHPROXY:

				jReq["method"] = "eth_submitLogin";
				if (m_worker.length()) jReq["worker"] = m_worker;
				jReq["params"].append(user + m_conn.Path());
				if (!m_email.empty()) jReq["params"].append(m_email);

				break;

			case EthStratumClient::ETHEREUMSTRATUM:

				jReq["params"].append("etcminer " + std::string(etcminer_get_buildinfo()->project_version));
				jReq["params"].append("EthereumStratum/1.0.0");

				break;
		}

		// Send first message
		sendSocketData(jReq);

		// Begin receive data
		recvSocketData();

	}

}

string EthStratumClient::processError(Json::Value& responseObject)
{
	string retVar = "";

	if (responseObject.isMember("error") && !responseObject.get("error", Json::Value::null).isNull()) {

		if (responseObject["error"].isConvertibleTo(Json::ValueType::stringValue)) {
			retVar = responseObject.get("error", "Unknown error").asString();
		}
		else if (responseObject["error"].isConvertibleTo(Json::ValueType::arrayValue))
		{
			for (auto i : responseObject["error"]) {
				retVar += i.asString() + " ";
			}
		}
		else if (responseObject["error"].isConvertibleTo(Json::ValueType::objectValue))
		{
			for (Json::Value::iterator i = responseObject["error"].begin(); i != responseObject["error"].end(); ++i)
			{
				Json::Value k = i.key();
				Json::Value v = (*i);
				retVar += (std::string)i.name() + ":" + v.asString() + " ";
			}
		}

	}
	else {
		retVar = "Unknown error";
	}

	return retVar;

}

void EthStratumClient::processExtranonce(std::string& enonce)
{
	m_extraNonceHexSize = enonce.length();

	cnote << "Extranonce set to " + enonce;

	for (int i = enonce.length(); i < 16; ++i) enonce += "0";
	m_extraNonce = h64(enonce);
}

void EthStratumClient::processReponse(Json::Value& responseObject)
{

	dev::setThreadName("stratum");
	
	int _rpcVer = 0;					// Store jsonrpc version to test against
	bool _isNotification = false;		// Wether or not this message is a reply to previous request or is a broadcast notification
	bool _isSuccess = false;			// Wether or not this is a succesful or failed response (implies _isNotification = false)
	string _errReason = "";				// Content of the error reason
	string _method = "";				// The method of the notification (or request from pool)
	int _id = 0;						// This SHOULD be the same id as the request it is responding to (known exception is ethermine.org using 999)

	if (!responseObject.isMember("jsonrpc")) {
		_rpcVer = 1;
	}
	else {
		_rpcVer = 2;
	}


	// Do some sanity checks over received json
	// Unfortunately most pool implementation is CRAP !

	switch (_rpcVer)
	{

	case 1:

		if (
			(!responseObject.isMember("result") && !responseObject.isMember("method")) ||
			(responseObject.isMember("method") && !responseObject.isMember("params"))
			)  {
			cwarn << "Pool sent an invalid jsonrpc (v1) response ...";
			cwarn << "Do not blame etcminer for this. Ask pool devs to honor http://www.jsonrpc.org/specification_v1 ";
			cwarn << "Disconnecting ...";
			disconnect();
			return;
		}


		// JsonRpc v1
		// http://www.jsonrpc.org/specification_v1#a1.2Response
		//
		// Members of message :
		// id - This MUST be the same id as the request it is responding to.
		// result - The Object that was returned by the invoked method.This must be null in case there was an error invoking the method.
		// error - An Error object if there was an error invoking the method.It must be null if there was no error.

		if (responseObject.isMember("result")) {

			_id = responseObject.get("id", 0).asUInt();
			_isSuccess = !responseObject.get("result", Json::Value::null).empty();
			_errReason = (_isSuccess ? "" : processError(responseObject));

		}
		
		// http://www.jsonrpc.org/specification_v1#a1.3Notification
		//
		// Members of message :
		// id - The request id MUST be null
		// method - A String containing the name of the method to be invoked.
		// params - An Array of objects to pass as arguments to the method.

		if (responseObject.isMember("method")) {

			_method = responseObject.get("method", "").asString();
			_id = responseObject.get("id", 0).asUInt();
			_isNotification = true;
			if (responseObject.get("method", Json::Value::null).empty()) {
				cwarn << "Missing \"method\" value in incoming notification. Discarding ...";
				return;
			}
			else if (!responseObject.isMember("params") || responseObject.get("params", Json::Value::null).empty()) {
				cwarn << "Missing \"params\" value in incoming notification. Discarding ...";
				return;
			}
			
		}


		break;

	case 2:

		// JsonRpc v2.0
		// http://www.jsonrpc.org/specification
		//
		// Members of message :
		//
		//	id
		//	This member is REQUIRED.
		//	It MUST be the same as the value of the id member in the Request Object.
		//	If there was an error in detecting the id in the Request object(e.g.Parse error / Invalid Request), it MUST be Null.
		//
		//  jsonrpc
		//	A String specifying the version of the JSON - RPC protocol.MUST be exactly "2.0".
		//
		//	result
		//	This member is REQUIRED on success.
		//	This member MUST NOT exist if there was an error invoking the method.
		//	The value of this member is determined by the method invoked on the Server.
		//
		//	error
		//	This member is REQUIRED on error.
		//	This member MUST NOT exist if there was no error triggered during invocation.
		//	The value for this member MUST be an Object as defined in section 5.1.

		if (
			(responseObject.isMember("error") && (responseObject.get("result", false).asBool() != false)) ||
			(responseObject.get("jsonrpc", "").asString() != "2.0") ||
			(responseObject.isMember("method") && (!responseObject.isMember("params") || responseObject.get("params", Json::Value::null).empty()))
			) {
			cwarn << "Pool sent an invalid jsonrpc (v2) response ...";
			cwarn << "Do not blame etcminer for this. Ask pool devs to honor http://www.jsonrpc.org/specification ";
			cwarn << "Disconnecting ...";
			disconnect();
			return;

		}

		// Responses
		if (!responseObject.isMember("method")) {

			_id = responseObject.get("id", 0).asUInt();
			_isSuccess = (!responseObject.isMember("error") || responseObject.get("error", Json::Value::null).isNull());
			_errReason = (_isSuccess ? "" : processError(responseObject));

		}

		// Notifications
		// A Notification is like a Request/Response object without an "id" member. 
		// But apparently all pools disregard this CRAP

		if (responseObject.isMember("method")) {

			_id = responseObject.get("id", 0).asUInt();
			_method = responseObject.get("method", "").asString();
			_isNotification = true;

			if (_method == "") {
				cwarn << "Missing \"method\" value in incoming notification. Discarding ...";
				return;
			}
			else if (!responseObject.isMember("params") || responseObject.get("params", Json::Value::null).empty()) {
				cwarn << "Missing \"params\" value in incoming notification. Discarding ...";
				return;
			}

		}

		break;

	default:

		// Should not get here but ready for implementation of possible future versions of jsonrpc
		cwarn << "Unrecognized jsonrpc syntax. Report the following data:";
		cwarn << responseObject;
		return;
		break;
	}


	// Handle awaited responses to OUR requests
	if (!_isNotification) {

		Json::Value jReq;
		Json::Value jPrm;

		switch (_id)
		{

		case 1:

			// Response to "mining.subscribe"
			switch (m_conn.Version()) {

			case EthStratumClient::STRATUM:

				m_subscribed.store(_isSuccess, std::memory_order_relaxed);
				if (!m_subscribed)
				{
					cnote << "Could not subscribe to stratum server";
					disconnect();
					return;
				}
				else {

					cnote << "Subscribed to stratum server";

					jReq["id"] = unsigned(3);
					jReq["jsonrpc"] = "2.0";
					jReq["method"] = "mining.authorize";
					jReq["params"] = Json::Value(Json::arrayValue);
					jReq["params"].append(m_conn.User() + m_conn.Path());
					jReq["params"].append(m_conn.Pass());


				}

				break;

			case EthStratumClient::ETHPROXY:

				m_subscribed.store(_isSuccess, std::memory_order_relaxed);
				if (!m_subscribed)
				{
					cnote << "Could not login to ethproxy server:" << _errReason;
					disconnect();
					return;
				}
				else {

					cnote << "Logged in to eth-proxy/etc-proxy server";
					m_authorized.store(true, std::memory_order_relaxed);

					jReq["id"] = unsigned(5);
					jReq["method"] = "eth_getWork";
					jReq["params"] = Json::Value(Json::arrayValue);

				}

				break;

			case EthStratumClient::ETHEREUMSTRATUM:

				m_subscribed.store(_isSuccess, std::memory_order_relaxed);
				if (!m_subscribed)
				{
					cnote << "Could not subscribe to stratum server:" << _errReason;
					disconnect();
					return;
				}
				else {

					cnote << "Subscribed to stratum server";

					m_nextWorkDifficulty = 1;
					jPrm = responseObject.get(((_rpcVer==1) ? "result":"params"), Json::Value::null);

					if (!jPrm.empty() && jPrm.isArray()) {
						std::string enonce = jPrm.get((Json::Value::ArrayIndex)1, "").asString();
						processExtranonce(enonce);
					}

					// Notify we're ready for extra nonce subscribtion on the fly
					// reply to this message should not perform any logic
					jReq["id"] = unsigned(2);
					jReq["method"] = "mining.extranonce.subscribe";
					jReq["params"] = Json::Value(Json::arrayValue);
					sendSocketData(jReq);

					// Eventually request authorization
					jReq["id"] = unsigned(3);
					jReq["method"] = "mining.authorize";
					jReq["params"].append(m_conn.User() + m_conn.Path());
					jReq["params"].append(m_conn.Pass());

				}


				break;
			}

			sendSocketData(jReq);
			break;

		case 2:

			// This is the reponse to mining.extranonce.subscribe
			// according to this 
			// https://github.com/nicehash/Specifications/blob/master/NiceHash_extranonce_subscribe_extension.txt
			// In all cases, client does not perform any logic when receiving back these replies.
			// With mining.extranonce.subscribe subscription, client should handle extranonce1
			// changes correctly

			// Nothing to do here.

			break;

		case 3:

			// Response to "mining.authorize"
			m_authorized.store(_isSuccess, std::memory_order_relaxed);

			if (!m_authorized)
			{
				cnote << "Worker not authorized" << m_conn.User() << _errReason;
				disconnect();
				return;
			
			}
			else {

				cnote << "Authorized worker " + m_conn.User();

			}
			
			break;

		case 4:

			// Response to solution submission mining.submit (4)
			{
				m_responsetimer.cancel();
				m_response_pending = false;
				if (_isSuccess) {
					if (m_onSolutionAccepted) {
						m_onSolutionAccepted(m_stale);
					}
				}
				else {
					cwarn << "Error :" + _errReason;
					if (m_onSolutionRejected) {
						m_onSolutionRejected(m_stale);
					}
				}
			}
			break;

		case 9:

			// Response to hashrate submit
			// Shall we do anyting ?
			if (!_isSuccess) {
				cwarn << "Submit hashRate failed:" << _errReason;
			}
			break;

		case 999:

			// This unfortunate case should not happen as none of the outgoing requests is marked with id 999
			// However it has been tested that ethermine.org responds with this id when error replying to 
			// either mining.subscribe (1) or mining.authorize requests (3)
			// To properly handle this situation we need to rely on Subscribed/Authorized states

			if (!_isSuccess) {

				if (!m_subscribed) {

					// Subscription pending
					cnote << "Subscription failed:" << _errReason;
					disconnect();
					return;

				}
				else if (m_subscribed && !m_authorized) {

					// Authorization pending
					cnote << "Worker not authorized:" << _errReason;
					disconnect();
					return;

				}
			};

			break;


		default:

			if (m_conn.Version() == EthStratumClient::ETHPROXY) {
				_method = "mining.notify";
				_isNotification = true;
			}
			else {

				// Never sent message with such an Id. What is it ?
				cnote << "Got response for unknown message id [" << _id << "] Discarding ...";

			}

			break;
		}

	}


	// Handle unsolicited messages FROM pool 
	// AKA notifications
	if (_isNotification) {

		Json::Value jReq;
		Json::Value jPrm;

		unsigned prmIdx;

		if (m_conn.Version() == EthStratumClient::ETHPROXY) {

			jPrm = responseObject.get("result", Json::Value::null);
			prmIdx = 0;
		}
		else
		{

			jPrm = responseObject.get("params", Json::Value::null);
			prmIdx = 1;

		}


		if (_method == "mining.notify")
		{

			if (jPrm.isArray())
			{
				string job = jPrm.get((Json::Value::ArrayIndex)0, "").asString();

				if (m_response_pending)
					m_stale = true;

				if (m_conn.Version() == EthStratumClient::ETHEREUMSTRATUM)
				{
					string sSeedHash = jPrm.get(1, "").asString();
					string sHeaderHash = jPrm.get(2, "").asString();

					if (sHeaderHash != "" && sSeedHash != "")
					{
						reset_work_timeout();

		                m_current.header = h256(sHeaderHash);
						m_current.seed = h256(sSeedHash);
						m_current.boundary = h256();
						diffToTarget((uint32_t*)m_current.boundary.data(), m_nextWorkDifficulty);
						m_current.startNonce = bswap(*((uint64_t*)m_extraNonce.data()));
						m_current.exSizeBits = m_extraNonceHexSize * 4;
						m_current.job_len = job.size();
						if (m_conn.Version() == EthStratumClient::ETHEREUMSTRATUM)
							job.resize(64, '0');
						m_current.job = h256(job);

						if (m_onWorkReceived) {
							m_onWorkReceived(m_current);
						}
					}
				}
				else
				{
					string sHeaderHash = jPrm.get((Json::Value::ArrayIndex)prmIdx++, "").asString();
					string sSeedHash = jPrm.get((Json::Value::ArrayIndex)prmIdx++, "").asString();
					string sShareTarget = jPrm.get((Json::Value::ArrayIndex)prmIdx++, "").asString();

					// coinmine.pl fix
					int l = sShareTarget.length();
					if (l < 66)
						sShareTarget = "0x" + string(66 - l, '0') + sShareTarget.substr(2);


					if (sHeaderHash != "" && sSeedHash != "" && sShareTarget != "")
					{
						h256 headerHash = h256(sHeaderHash);

						if (headerHash != m_current.header)
						{
							reset_work_timeout();

							m_current.header = h256(sHeaderHash);
							m_current.seed = h256(sSeedHash);
							m_current.boundary = h256(sShareTarget);
							m_current.job = h256(job);

							if (m_onWorkReceived) {
								m_onWorkReceived(m_current);
							}
						}
					}
				}
			}
		}
		else if (_method == "mining.set_difficulty" && m_conn.Version() == EthStratumClient::ETHEREUMSTRATUM)
		{
			jPrm = responseObject.get("params", Json::Value::null);
			if (jPrm.isArray())
			{
				m_nextWorkDifficulty = jPrm.get((Json::Value::ArrayIndex)0, 1).asDouble();
				if (m_nextWorkDifficulty <= 0.0001) m_nextWorkDifficulty = 0.0001;
				cnote << "Difficulty set to"  << m_nextWorkDifficulty;
			}
		}
		else if (_method == "mining.set_extranonce" && m_conn.Version() == EthStratumClient::ETHEREUMSTRATUM)
		{
			jPrm = responseObject.get("params", Json::Value::null);
			if (jPrm.isArray())
			{
				std::string enonce = jPrm.get((Json::Value::ArrayIndex)0, "").asString();
				processExtranonce(enonce);
			}
		}
		else if (_method == "client.get_version")
		{

			jReq["id"] = toString(_id);
			jReq["result"] = etcminer_get_buildinfo()->project_version;

			if (_rpcVer == 1) {
				jReq["error"] = Json::Value::null;
			}
			else if (_rpcVer == 2) {
				jReq["jsonrpc"] = "2.0";
			}

			sendSocketData(jReq);
		}
		else {

			cwarn << "Got unknown method [" << _method << "] from pool. Discarding ...";

		}


	}

}

void EthStratumClient::work_timeout_handler(const boost::system::error_code& ec) {

	dev::setThreadName("stratum");
	m_worktimer.cancel();

	if (!ec) {
		if (isConnected()) {
			cwarn << "No new work received in " << m_worktimeout << " seconds.";
			disconnect();
		}
	}

}

void EthStratumClient::response_timeout_handler(const boost::system::error_code& ec) {

	dev::setThreadName("stratum");

	if (!ec) {
		if (isConnected() && m_response_pending) {
			cwarn << "No response received in" << m_responsetimeout << "seconds.";
			disconnect();
		}
	}

}

void EthStratumClient::submitHashrate(string const & rate) {
	
	m_rate = rate;
	
	if (!m_submit_hashrate || !isConnected()) {
		return;
	}

	// There is no stratum method to submit the hashrate so we use the rpc variant.
	// Note !!
	// id = 6 is also the id used by ethermine.org and nanopool to push new jobs
	// thus we will be in trouble if we want to check the result of hashrate submission
	// actually change the id from 6 to 9

	Json::Value jReq;
	jReq["id"] = unsigned(9);
	jReq["jsonrpc"] = "2.0";
	if (m_worker.length()) jReq["worker"] = m_worker;
	jReq["method"] = "eth_submitHashrate";
	jReq["params"] = Json::Value(Json::arrayValue);
	jReq["params"].append(m_rate);
	jReq["params"].append("0x" + toString(this->m_submit_hashrate_id));

	sendSocketData(jReq);

}

void EthStratumClient::submitSolution(Solution solution) {

	string nonceHex = toHex(solution.nonce);

	m_responsetimer.cancel();
	m_responsetimer.expires_from_now(boost::posix_time::seconds(m_responsetimeout));
	m_responsetimer.async_wait(boost::bind(&EthStratumClient::response_timeout_handler, this, boost::asio::placeholders::error));

	Json::Value jReq;

	jReq["id"] = unsigned(4);
	jReq["method"] = "mining.submit";
	jReq["params"] = Json::Value(Json::arrayValue);

	switch (m_conn.Version()) {

		case EthStratumClient::STRATUM:
			
			jReq["jsonrpc"] = "2.0";
			jReq["params"].append(m_conn.User());
			jReq["params"].append(solution.work.job.hex());
			jReq["params"].append("0x" + nonceHex);
			jReq["params"].append("0x" + solution.work.header.hex());
			jReq["params"].append("0x" + solution.mixHash.hex());
			if (m_worker.length()) jReq["worker"] = m_worker;

			break;

		case EthStratumClient::ETHPROXY:

			jReq["method"] = "eth_submitWork";
			jReq["params"].append("0x" + nonceHex);
			jReq["params"].append("0x" + solution.work.header.hex());
			jReq["params"].append("0x" + solution.mixHash.hex());
			if (m_worker.length()) jReq["worker"] = m_worker;

			break;

		case EthStratumClient::ETHEREUMSTRATUM:

			jReq["params"].append(m_conn.User());
			jReq["params"].append(solution.work.job.hex().substr(0, solution.work.job_len));
			jReq["params"].append(nonceHex.substr(m_extraNonceHexSize, 16 - m_extraNonceHexSize));

			break;

	}

	sendSocketData(jReq);

	m_stale = solution.stale;
	m_response_pending = true;

}

void EthStratumClient::recvSocketData() {
	
	if (m_conn.SecLevel() != SecureLevel::NONE) {

		async_read_until(*m_securesocket, m_recvBuffer, "\n",
			boost::bind(&EthStratumClient::onRecvSocketDataCompleted, this,
				boost::asio::placeholders::error, boost::asio::placeholders::bytes_transferred));
	}
	else {

		async_read_until(*m_nonsecuresocket, m_recvBuffer, "\n",
			boost::bind(&EthStratumClient::onRecvSocketDataCompleted, this,
				boost::asio::placeholders::error, boost::asio::placeholders::bytes_transferred));
	}

}

void EthStratumClient::onRecvSocketDataCompleted(const boost::system::error_code& ec, std::size_t bytes_transferred) {
	
	dev::setThreadName("stratum");

	if (!ec && bytes_transferred > 0) {

		// Extract received message
		std::istream is(&m_recvBuffer);
		std::string message;
		getline(is, message);

		if (!message.empty()) {

			// Test validity of chunk and process
			Json::Value jMsg;
			Json::Reader jRdr;
			if (jRdr.parse(message, jMsg)) {
				processReponse(jMsg);
			}
			else {
				cwarn << "Got invalid Json message :" + jRdr.getFormattedErrorMessages();
			}

		}

		// Eventually keep reading from socket
		if (isConnected()) { recvSocketData(); }

	}
	else
	{
		if (isConnected()) {
			cwarn << "Socket read failed:" << ec.message();
			disconnect();
		}
	}

}

void EthStratumClient::sendSocketData(Json::Value const & jReq) {


	if (!isConnected())
		return;
	
	std::ostream os(&m_sendBuffer);
	os << m_jWriter.write(jReq);		// Do not add lf. It's added by writer.
	
	if (m_conn.SecLevel() != SecureLevel::NONE) {

		async_write(*m_securesocket, m_sendBuffer,
			boost::bind(&EthStratumClient::onSendSocketDataCompleted, this, boost::asio::placeholders::error));

	}
	else {

		async_write(*m_nonsecuresocket, m_sendBuffer,
			boost::bind(&EthStratumClient::onSendSocketDataCompleted, this, boost::asio::placeholders::error));

	}

}

void EthStratumClient::onSendSocketDataCompleted(const boost::system::error_code& ec) {

	dev::setThreadName("stratum");

	if (ec) {
		if (isConnected()) {
			cwarn << "Socket write failed: " + ec.message();
			disconnect();
		}
	}

}
