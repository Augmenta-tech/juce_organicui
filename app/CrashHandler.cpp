/*
  ==============================================================================

	CrashHandler.cpp
	Created: 29 Oct 2017 1:40:04pm
	Author:  Ben

  ==============================================================================
*/

#include "JuceHeader.h"
#include "CrashHandler.h"

#include <ctime>
#if JUCE_LINUX
#include <sys/utsname.h>
#endif

#if JUCE_WINDOWS
#include <windows.h> 
#include <DbgHelp.h>
#include <tchar.h>
#endif

#if JUCE_WINDOWS
LONG WINAPI handleCrashStatic(LPEXCEPTION_POINTERS e);
void createDumpAndStrackTrace(void* e, File dumpFile, File traceFile);
#else
void handleCrashStatic(int signum);
void createDumpAndStrackTrace(int signum, File dumpFile, File traceFile);
#endif

juce_ImplementSingleton(CrashDumpUploader)
OrganicApplication::MainWindow* getMainWindow();

CrashDumpUploader::CrashDumpUploader() :
	Thread("Crashdump"),
	progress("Upload Progress", "", 0, 0, 1)
{

}

CrashDumpUploader::~CrashDumpUploader()
{
}

void CrashDumpUploader::init(const String& url, Image image)
{
	remoteURL = URL(url);
	crashImage = image;

	SystemStats::setApplicationCrashHandler((SystemStats::CrashHandlerFunction)handleCrashStatic);
}

#if JUCE_WINDOWS
LONG WINAPI handleCrashStatic(LPEXCEPTION_POINTERS e)
#else
void handleCrashStatic(int e)
#endif
{

#if JUCE_WINDOWS
	CrashDumpUploader::getInstance()->handleCrash(e);
	return EXCEPTION_EXECUTE_HANDLER;
#else
	CrashDumpUploader::getInstance()->handleCrash(e);
#endif
}

#if JUCE_WINDOWS
void CrashDumpUploader::handleCrash(void* e)
#else
void CrashDumpUploader::handleCrash(int e)
#endif
{
	//create recovered file 
	File f = Engine::mainEngine->getFile();

	recoveredFile = f.existsAsFile() ? f.getParentDirectory().getChildFile(f.getFileNameWithoutExtension() + "_recovered" + f.getFileExtension()) : File::getSpecialLocation(File::userDocumentsDirectory).getChildFile(getApp().appProperties->getStorageParameters().applicationName + "/recovered_session" + Engine::mainEngine->fileExtension);

	if (recoveredFile.existsAsFile()) recoveredFile.deleteFile();

	var data = Engine::mainEngine->getJSONData();
	std::unique_ptr<OutputStream> os(recoveredFile.createOutputStream());
	if (os != nullptr)
	{
		JSON::writeToStream(*os, data, false);
		os->flush();
	}


	LOGERROR("A Crash happened !");
	LOGERROR(SystemStats::getStackBacktrace());

	//Let the app handle app-specific actions for crash
	getApp().handleCrashed();


	crashAction = GlobalSettings::getInstance()->actionOnCrash->getValueDataAsEnum<GlobalSettings::CrashAction>();


	traceFile = recoveredFile.getParentDirectory().getChildFile("crashlog.txt");

#if JUCE_WINDOWS
	dumpFile = recoveredFile.getParentDirectory().getChildFile("crashlog.dmp");
#else
	dumpFile = File();
#endif

	if (traceFile.existsAsFile()) traceFile.deleteFile();
	if (dumpFile.existsAsFile()) dumpFile.deleteFile();

	createDumpAndStrackTrace(e, dumpFile, traceFile);


	if (getApp().useWindow && crashAction == GlobalSettings::REPORT)
	{
		w.reset(new UploadWindow());
		DialogWindow::showDialog("Got crashed ?", w.get(), getMainWindow(), Colours::black, true);

		MessageManager::getInstance()->runDispatchLoop();

		exitApp();

	}
	else
	{
		if (GlobalSettings::getInstance()->autoSendCrashLog->boolValue())
		{
			LOG("Uploading crash...");
			uploadCrash();
		}

		exitApp();
	}
}

void CrashDumpUploader::uploadCrash()
{
	if (remoteURL.isEmpty())
	{
		LOGWARNING("Crash dump upload url has not been assigned");
		dumpFile.deleteFile();
		traceFile.deleteFile();
		return;
	}

	if (uploadReport("crash", crashMessage.isNotEmpty() ? crashMessage : "No message",
		{}, recoveredFile, true))
	{
		uploadPendingDiagnostics();
	}

	sleep(300);

	if (w != nullptr)
	{
		if (DialogWindow* dw = w->findParentComponentOfClass<DialogWindow>()) dw->exitModalState(0);
		MessageManagerLock mmLock;
		w->removeFromDesktop();
		w.reset();
	}
}

bool CrashDumpUploader::uploadReport(const String& reportType,
	const String& message,
	const Array<File>& diagnosticFiles,
	File sessionFile,
	bool includeCrashArtifacts,
	const String& reportId)
{
	if (remoteURL.isEmpty())
		return false;

	const auto currentTime = Time::getCurrentTime();
	String timezone = currentTime.getTimeZone();
	String osName = SystemStats::getOperatingSystemName();
#if JUCE_LINUX
	const File timezoneFile("/etc/timezone");
	if (timezoneFile.existsAsFile())
		timezone = timezoneFile.loadFileAsString().trim();

	const File osReleaseFile("/etc/os-release");
	if (osReleaseFile.existsAsFile())
	{
		StringArray lines;
		lines.addLines(osReleaseFile.loadFileAsString());
		for (const auto& line : lines)
		{
			if (!line.startsWith("PRETTY_NAME="))
				continue;

			auto value = line.fromFirstOccurrenceOf("=", false, false).trim();
			if (value.length() >= 2
				&& ((value.startsWithChar('"') && value.endsWithChar('"'))
					|| (value.startsWithChar('\'') && value.endsWithChar('\''))))
				value = value.substring(1, value.length() - 1);

			if (value.isNotEmpty())
				osName = value;
			break;
		}
	}
#endif
	const String channel =
#if JUCE_DEBUG
		"debug";
#else
		getAppVersion().containsChar('b') ? "beta" : "stable";
#endif

	std::time_t utcTime = std::time(nullptr);
	std::tm utc = {};
#if JUCE_WINDOWS
	gmtime_s(&utc, &utcTime);
#else
	gmtime_r(&utcTime, &utc);
#endif
	char utcBuffer[32] = {};
	std::strftime(utcBuffer, sizeof(utcBuffer), "%Y-%m-%dT%H:%M:%SZ", &utc);

	var metadata(new DynamicObject());
	auto* metadataObject = metadata.getDynamicObject();
	metadataObject->setProperty("schema_version", 1);
	metadataObject->setProperty("report_type", reportType);
	if (reportId.isNotEmpty())
		metadataObject->setProperty("report_id", reportId);
	metadataObject->setProperty("timestamp_utc", String(utcBuffer));
	metadataObject->setProperty("timestamp_local", currentTime.toISO8601(true));
	metadataObject->setProperty("username", SystemStats::getFullUserName());
	metadataObject->setProperty("hostname", SystemStats::getComputerName());
	metadataObject->setProperty("timezone", timezone);
	metadataObject->setProperty("utc_offset", currentTime.getUTCOffsetString(true));

	var application(new DynamicObject());
	application.getDynamicObject()->setProperty("version", getAppVersion());
	application.getDynamicObject()->setProperty("channel", channel);
#ifdef AUGMENTA_BUILD_NUMBER
	application.getDynamicObject()->setProperty("build_number", AUGMENTA_BUILD_NUMBER);
#endif
	metadataObject->setProperty("application", application);

	var system(new DynamicObject());
	system.getDynamicObject()->setProperty("os", osName);
#if JUCE_LINUX
	struct utsname uts = {};
	if (uname(&uts) == 0)
	{
		system.getDynamicObject()->setProperty("kernel", String(uts.release));
		system.getDynamicObject()->setProperty("architecture", String(uts.machine));
	}
#endif
	metadataObject->setProperty("system", system);

	URL url = remoteURL.withParameter("username", SystemStats::getFullUserName().replace(" ", "-"))
		.withParameter("os", osName.replace(" ", "-"))
		.withParameter("version", getAppVersion())
		.withParameter("message", message)
		.withParameter("email", contactEmail.isNotEmpty() ? contactEmail : "")
		.withParameter("test", (reportType == "crash" && isTestCrash) ? "1" : "0")
		.withParameter("report_type", reportType)
		.withParameter("report_id", reportId)
		.withParameter("metadata", JSON::toString(metadata, true))
		.withParameter("branch", channel);

	if (includeCrashArtifacts && dumpFile.existsAsFile())
		url = url.withFileToUpload("dumpFile", dumpFile, "application/octet-stream");

	if (includeCrashArtifacts && traceFile.existsAsFile())
		url = url.withFileToUpload("traceFile", traceFile, "text/plain");

	if (sessionFile.existsAsFile())
		url = url.withFileToUpload("sessionFile", sessionFile, "application/octet-stream");

	for (int i = 0; i < diagnosticFiles.size(); ++i)
	{
		const auto& file = diagnosticFiles.getReference(i);
		if (file.existsAsFile())
			url = url.withFileToUpload(String("diagnosticFile") + String(i), file, "application/gzip");
	}

	std::function<bool(int, int)> callbackFunc = std::bind(
		&CrashDumpUploader::openStreamProgressCallback, this,
		std::placeholders::_1, std::placeholders::_2);
	int statusCode = 0;

	URL::InputStreamOptions options = URL::InputStreamOptions(URL::ParameterHandling::inPostData)
		.withExtraHeaders("Cache-Control: no-cache")
		.withProgressCallback(callbackFunc)
		.withStatusCode(&statusCode)
		.withConnectionTimeoutMs(5000);

	std::unique_ptr<InputStream> stream(URL(url).createInputStream(options));
	if (stream == nullptr || (statusCode != 0 && (statusCode < 200 || statusCode >= 300)))
	{
		LOGWARNING("Failed to upload " + reportType + " report, status code = " + String(statusCode));
		return false;
	}

	const String response = stream->readEntireStreamAsString();
#if JUCE_DEBUG
	LOG("Received : " << response);
#endif
	if (response.trim() != "ok")
	{
		LOGWARNING("Error from diagnostic report server: " + response);
		return false;
	}

	LOG(reportType + " report uploaded successfully");
	return true;
}

void CrashDumpUploader::uploadPendingDiagnostics()
{
	if (!diagnosticFilesProvider)
		return;

	const auto files = diagnosticFilesProvider();
	for (const auto& file : files)
	{
		if (!file.existsAsFile())
			continue;

		Array<File> singleFile;
		singleFile.add(file);
		const File session = diagnosticSessionProvider ? diagnosticSessionProvider() : File();
		const String reportId = file.getParentDirectory().getFileName();
		if (uploadReport("freeze", "Automatic watchdog freeze diagnostic", singleFile, session, false, reportId)
			&& diagnosticFilesSentCallback)
		{
			diagnosticFilesSentCallback(singleFile);
		}
	}
}

bool CrashDumpUploader::openStreamProgressCallback(int bytesDownloaded, int totalLength)
{
	progress.setValue(bytesDownloaded * 1.0f / totalLength);
	LOG("Progress " << (int)(progress.floatValue() * 100) << "%");
	return !threadShouldExit();
}

void CrashDumpUploader::exitApp()
{
	File curFile;

	if (Engine::mainEngine != nullptr)
	{
		curFile = Engine::mainEngine->getFile();
		Engine::mainEngine->clear();
	}

	if (crashAction == GlobalSettings::RECOVER && recoveredFile.exists())
	{
		File::getSpecialLocation(File::currentApplicationFile).startAsProcess("-c \"" + recoveredFile.getFullPathName() + "\"");
	}
	else if (crashAction == GlobalSettings::REOPEN && curFile.exists())
	{
		File::getSpecialLocation(File::currentApplicationFile).startAsProcess("-c \"" + curFile.getFullPathName() + "\"");
	}

	getApp().quit();
}

void CrashDumpUploader::run()
{
	uploadCrash();
	MessageManager::getInstance()->stopDispatchLoop();
}

#if JUCE_WINDOWS
void createDumpAndStrackTrace(void* ev, File dumpFile, File traceFile)
#else
void createDumpAndStrackTrace(int signum, File dumpFile, File traceFile)
#endif
{

#if JUCE_WINDOWS

	LPEXCEPTION_POINTERS exceptionPointers = (LPEXCEPTION_POINTERS)ev;

	HANDLE hFile = CreateFile(dumpFile.getFullPathName().getCharPointer(), GENERIC_READ | GENERIC_WRITE,
		0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);

	if (hFile != nullptr && hFile != INVALID_HANDLE_VALUE)
	{
		MINIDUMP_EXCEPTION_INFORMATION exceptionInformation;

		exceptionInformation.ThreadId = GetCurrentThreadId();
		exceptionInformation.ExceptionPointers = exceptionPointers;
		exceptionInformation.ClientPointers = FALSE;

		MINIDUMP_TYPE dumpType = MiniDumpNormal;

		BOOL dumpWriteResult = MiniDumpWriteDump(GetCurrentProcess(),
			GetCurrentProcessId(),
			hFile,
			dumpType,
			exceptionPointers != nullptr ? &exceptionInformation : 0,
			nullptr,
			nullptr);

		if (!dumpWriteResult)
			_tprintf(_T("MiniDumpWriteDump failed. Error: %u \n"), GetLastError());
		else
			_tprintf(_T("Minidump created.\n"));

		CloseHandle(hFile);
	}
	else
	{
		_tprintf(_T("CreateFile failed. Error: %u \n"), GetLastError());
	}
#endif


	//Stack trace

	DBG("Create Stack trace here !");
	String stackTrace = SystemStats::getStackBacktrace();

	FileOutputStream fos(traceFile);
	if (fos.openedOk())
	{
		fos.writeText(stackTrace, false, false, "\n");
		fos.flush();
	}
}


CrashDumpUploader::UploadWindow::UploadWindow() :
	okBT("Send and close"),
	cancelBT("Close only"),
	autoReopenBT("Send and recover"),
	recoverOnlyBT("Recover Only"),
	progressUI(&CrashDumpUploader::getInstance()->progress)
{
	okBT.addListener(this);
	addAndMakeVisible(&okBT);

	cancelBT.addListener(this);
	addAndMakeVisible(&cancelBT);

#if !JUCE_MAC
	recoverOnlyBT.addListener(this);
	addAndMakeVisible(&recoverOnlyBT);

	autoReopenBT.addListener(this);
	addAndMakeVisible(&autoReopenBT);
#endif

	addAndMakeVisible(&progressUI);

	mail.setMultiLine(false);
	mail.setColour(mail.backgroundColourId, BG_COLOR.brighter(.3f));
	mail.setColour(mail.textColourId, TEXT_COLOR.brighter());
	mail.setColour(mail.outlineColourId, BG_COLOR.brighter(.6f));
	mail.setText(GlobalSettings::getInstance()->crashContactEmail->stringValue(), dontSendNotification);
	mail.setTextToShowWhenEmpty("Your contact email if you accept to be contacted to help fix the problem (and only that !).", TEXT_COLOR);

	addAndMakeVisible(mail);

	addAndMakeVisible(&editor);
	editor.setColour(editor.backgroundColourId, BG_COLOR.brighter(.3f));
	editor.setColour(editor.textColourId, TEXT_COLOR.brighter());
	editor.setColour(editor.outlineColourId, BG_COLOR.brighter(.6f));
	editor.setTextToShowWhenEmpty("Your super comprehensive yet fun explanation here. You can write love messages as well, but only if you mean it.", TEXT_COLOR);
	editor.setMultiLine(true);
	editor.setReturnKeyStartsNewLine(true);


	setSize(800, 600);

}

CrashDumpUploader::UploadWindow::~UploadWindow()
{

}

void CrashDumpUploader::UploadWindow::paint(Graphics& g)
{
	g.drawImage(CrashDumpUploader::getInstance()->crashImage, getLocalBounds().toFloat());
}

void CrashDumpUploader::UploadWindow::resized()
{
	juce::Rectangle<int> r = getLocalBounds().removeFromBottom(getHeight() / 2);
	juce::Rectangle<int> br = r.removeFromBottom(30).reduced(2);
#if !JUCE_MAC
	autoReopenBT.setBounds(br.removeFromRight(100));
	br.removeFromRight(8);
	recoverOnlyBT.setBounds(br.removeFromRight(100));
	br.removeFromRight(8);
#endif
	okBT.setBounds(br.removeFromRight(100));
	br.removeFromRight(8);
	cancelBT.setBounds(br.removeFromRight(100));

	progressUI.setBounds(r.removeFromBottom(30).reduced(20, 5));

	mail.setBounds(r.removeFromTop(30).reduced(20, 0));

	editor.setBounds(r.reduced(20));
}

void CrashDumpUploader::UploadWindow::buttonClicked(Button* bt)
{
	okBT.setEnabled(false);
	cancelBT.setEnabled(false);

#if !JUCE_MAC
	autoReopenBT.setEnabled(false);
	recoverOnlyBT.setEnabled(false);
#endif

	CrashDumpUploader::getInstance()->uploadFile = bt == &autoReopenBT || bt == &okBT;
	CrashDumpUploader::getInstance()->crashMessage = editor.getText();
	CrashDumpUploader::getInstance()->contactEmail = mail.getText();
	CrashDumpUploader::getInstance()->crashAction = (bt == &autoReopenBT || bt == &recoverOnlyBT) ? GlobalSettings::RECOVER : GlobalSettings::KILL;


	if (mail.getText().isNotEmpty() && mail.getText() != GlobalSettings::getInstance()->crashContactEmail->stringValue())
	{
		GlobalSettings::getInstance()->crashContactEmail->setValue(mail.getText());
		getApp().saveGlobalSettings();
	}

	if (bt == &autoReopenBT || bt == &okBT)
	{
		CrashDumpUploader::getInstance()->startThread();
	}
	else if (bt == &cancelBT || bt == &recoverOnlyBT)
	{
		if (DialogWindow* dw = findParentComponentOfClass<DialogWindow>()) dw->exitModalState(0);
		MessageManager::getInstance()->stopDispatchLoop();
	}


	/*if (DialogWindow* dw = findParentComponentOfClass<DialogWindow>())
		dw->exitModalState(0);
		*/
}
