#pragma once

class CrashDumpUploader : 
	public juce::Thread
{
public:
	juce_DeclareSingleton(CrashDumpUploader, true);

	CrashDumpUploader();
	~CrashDumpUploader();

	juce::URL remoteURL;
	juce::Image crashImage;

	bool uploadFile;
	bool isTestCrash = false;
	GlobalSettings::CrashAction crashAction;

	juce::File traceFile;
	juce::File dumpFile;
	juce::File recoveredFile;
	juce::String contactEmail;
	juce::String crashMessage;
	FloatParameter progress;

	void init(const juce::String& url, juce::Image image);

#if JUCE_WINDOWS
	void handleCrash(void * e);
#else
	void handleCrash(int signum);
#endif

	void run();
	void uploadCrash();
	bool uploadReport(const juce::String& reportType,
		const juce::String& message,
		const juce::Array<juce::File>& diagnosticFiles = {},
		juce::File sessionFile = {},
		bool includeCrashArtifacts = false,
		const juce::String& reportId = {},
		juce::var sourceMetadata = {});
	void uploadPendingDiagnostics();

	void setDiagnosticFilesProvider(std::function<juce::Array<juce::File>()> provider) { diagnosticFilesProvider = provider; }
	void setDiagnosticSessionProvider(std::function<juce::File()> provider) { diagnosticSessionProvider = provider; }
	void setDiagnosticFilesSentCallback(std::function<void(const juce::Array<juce::File>&)> callback) { diagnosticFilesSentCallback = callback; }

	bool openStreamProgressCallback(int /*bytesSent*/, int /*totalBytes*/);

	void exitApp();

	class UploadWindow :
		public juce::Component,
		public juce::Button::Listener
	{
	public:
		UploadWindow();
		~UploadWindow();

		juce::TextEditor mail;
		juce::TextEditor editor;
		juce::Image* image;
		juce::TextButton okBT;
		juce::TextButton cancelBT;
		juce::TextButton autoReopenBT;
		juce::TextButton recoverOnlyBT;
		FloatSliderUI progressUI;

		juce::Rectangle<int> imageRect;

		void paint(juce::Graphics& g) override;
		void resized() override;
		
		void buttonClicked(juce::Button* bt) override;

		JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(UploadWindow)
	};

	std::unique_ptr<UploadWindow> w;

	std::function<juce::Array<juce::File>()> diagnosticFilesProvider;
	std::function<juce::File()> diagnosticSessionProvider;
	std::function<void(const juce::Array<juce::File>&)> diagnosticFilesSentCallback;

	JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(CrashDumpUploader)

};