using System.Buffers.Binary;
using System.Diagnostics;
using System.IO;
using System.IO.Pipes;
using System.Threading.Channels;
using NAudio.CoreAudioApi;
using NAudio.Wave;

namespace VdsGui;

public sealed class AudioHapticsEngine : IAsyncDisposable
{
    private const string PipeName = "vdsd-audio";
    private const int HapticsSampleRate = 48_000;
    private const int FramesPerChunk = 512;
    private const int BytesPerChunk = FramesPerChunk * 2 * sizeof(short);
    private readonly object _lifecycleLock = new();
    private readonly HapticsProcessor _processor = new();
    private MMDeviceEnumerator? _enumerator;
    private MMDevice? _device;
    private WasapiLoopbackCapture? _capture;
    private NamedPipeClientStream? _pipe;
    private Channel<byte[]>? _chunks;
    private CancellationTokenSource? _cancellation;
    private Task? _writerTask;
    private AudioHapticsSettings _settings = new();
    private long _lastLevelsTimestamp;
    private bool _running;

    public bool IsRunning
    {
        get
        {
            lock (_lifecycleLock)
            {
                return _running;
            }
        }
    }

    public event EventHandler<AudioHapticsLevels>? LevelsChanged;
    public event EventHandler<string>? Faulted;

    public static IReadOnlyList<AudioRenderDevice> GetRenderDevices()
    {
        using var enumerator = new MMDeviceEnumerator();
        string? defaultId = null;
        try
        {
            defaultId = enumerator.GetDefaultAudioEndpoint(DataFlow.Render, Role.Multimedia).ID;
        }
        catch
        {
        }
        return enumerator.EnumerateAudioEndPoints(DataFlow.Render, DeviceState.Active)
            .Select(device => new AudioRenderDevice(
                device.ID,
                device.FriendlyName,
                string.Equals(device.ID, defaultId, StringComparison.OrdinalIgnoreCase)))
            .OrderByDescending(device => device.IsDefault)
            .ThenBy(device => device.Name, StringComparer.CurrentCultureIgnoreCase)
            .ToArray();
    }

    public void UpdateSettings(AudioHapticsSettings settings)
    {
        Volatile.Write(ref _settings, settings.Clone());
    }

    public async Task StartAsync(AudioHapticsSettings settings, CancellationToken cancellationToken = default)
    {
        lock (_lifecycleLock)
        {
            if (_running)
            {
                return;
            }
            _running = true;
        }

        try
        {
            UpdateSettings(settings);
            _enumerator = new MMDeviceEnumerator();
            _device = ResolveDevice(_enumerator, settings.DeviceId);
            _capture = new WasapiLoopbackCapture(_device);
            _capture.DataAvailable += Capture_DataAvailable;
            _capture.RecordingStopped += Capture_RecordingStopped;
            _processor.Reset(_capture.WaveFormat);
            _chunks = Channel.CreateBounded<byte[]>(new BoundedChannelOptions(20)
            {
                FullMode = BoundedChannelFullMode.DropOldest,
                SingleReader = true,
                SingleWriter = true
            });
            _cancellation = CancellationTokenSource.CreateLinkedTokenSource(cancellationToken);
            _pipe = new NamedPipeClientStream(
                ".",
                PipeName,
                PipeDirection.Out,
                PipeOptions.Asynchronous | PipeOptions.WriteThrough);
            await _pipe.ConnectAsync(2_500, _cancellation.Token);
            await WriteHeaderAsync(_pipe, _cancellation.Token);
            _writerTask = WriteChunksAsync(_pipe, _chunks.Reader, _cancellation.Token);
            _capture.StartRecording();
        }
        catch
        {
            await StopCoreAsync(false);
            throw;
        }
    }

    public async Task StopAsync() => await StopCoreAsync(true);

    public async ValueTask DisposeAsync() => await StopCoreAsync(false);

    private static MMDevice ResolveDevice(MMDeviceEnumerator enumerator, string id)
    {
        if (!string.IsNullOrWhiteSpace(id))
        {
            try
            {
                return enumerator.GetDevice(id);
            }
            catch
            {
            }
        }
        return enumerator.GetDefaultAudioEndpoint(DataFlow.Render, Role.Multimedia);
    }

    private async Task StopCoreAsync(bool sendSilence)
    {
        WasapiLoopbackCapture? capture;
        Channel<byte[]>? chunks;
        Task? writerTask;
        CancellationTokenSource? cancellation;
        NamedPipeClientStream? pipe;
        lock (_lifecycleLock)
        {
            if (!_running && _capture is null && _pipe is null)
            {
                return;
            }
            _running = false;
            capture = _capture;
            chunks = _chunks;
            writerTask = _writerTask;
            cancellation = _cancellation;
            pipe = _pipe;
            _capture = null;
            _chunks = null;
            _writerTask = null;
            _cancellation = null;
            _pipe = null;
        }

        if (capture is not null)
        {
            capture.DataAvailable -= Capture_DataAvailable;
            capture.RecordingStopped -= Capture_RecordingStopped;
            try
            {
                capture.StopRecording();
            }
            catch
            {
            }
            capture.Dispose();
        }

        if (chunks is not null)
        {
            if (sendSilence)
            {
                chunks.Writer.TryWrite(new byte[BytesPerChunk]);
                chunks.Writer.TryWrite(new byte[BytesPerChunk]);
            }
            chunks.Writer.TryComplete();
        }
        if (writerTask is not null)
        {
            try
            {
                await writerTask;
            }
            catch
            {
            }
        }
        cancellation?.Cancel();
        cancellation?.Dispose();
        pipe?.Dispose();
        _device?.Dispose();
        _device = null;
        _enumerator?.Dispose();
        _enumerator = null;
    }

    private async Task WriteChunksAsync(
        Stream stream,
        ChannelReader<byte[]> reader,
        CancellationToken cancellationToken)
    {
        try
        {
            await foreach (var chunk in reader.ReadAllAsync(cancellationToken))
            {
                await stream.WriteAsync(chunk, cancellationToken);
            }
            await stream.FlushAsync(cancellationToken);
        }
        catch (OperationCanceledException) when (cancellationToken.IsCancellationRequested)
        {
        }
        catch (Exception error)
        {
            Faulted?.Invoke(this, $"音频触觉连接中断：{error.Message}");
        }
    }

    private static async Task WriteHeaderAsync(Stream stream, CancellationToken cancellationToken)
    {
        var header = new byte[16];
        BinaryPrimitives.WriteUInt32LittleEndian(header.AsSpan(0, 4), 0x41534456);
        BinaryPrimitives.WriteUInt16LittleEndian(header.AsSpan(4, 2), 1);
        BinaryPrimitives.WriteUInt16LittleEndian(header.AsSpan(6, 2), 2);
        BinaryPrimitives.WriteUInt32LittleEndian(header.AsSpan(8, 4), HapticsSampleRate);
        BinaryPrimitives.WriteUInt32LittleEndian(header.AsSpan(12, 4), FramesPerChunk);
        await stream.WriteAsync(header, cancellationToken);
        await stream.FlushAsync(cancellationToken);
    }

    private void Capture_DataAvailable(object? sender, WaveInEventArgs eventArgs)
    {
        try
        {
            var capture = _capture;
            var writer = _chunks?.Writer;
            if (capture is null || writer is null)
            {
                return;
            }
            var settings = Volatile.Read(ref _settings);
            var result = _processor.Process(eventArgs.Buffer, eventArgs.BytesRecorded, capture.WaveFormat, settings);
            foreach (var chunk in result.Chunks)
            {
                writer.TryWrite(chunk);
            }
            var timestamp = Stopwatch.GetTimestamp();
            if (timestamp - Interlocked.Read(ref _lastLevelsTimestamp) >= Stopwatch.Frequency / 25)
            {
                Interlocked.Exchange(ref _lastLevelsTimestamp, timestamp);
                LevelsChanged?.Invoke(this, result.Levels);
            }
        }
        catch (Exception error)
        {
            Faulted?.Invoke(this, $"音频触觉处理失败：{error.Message}");
        }
    }

    private void Capture_RecordingStopped(object? sender, StoppedEventArgs eventArgs)
    {
        if (eventArgs.Exception is not null && IsRunning)
        {
            Faulted?.Invoke(this, $"WASAPI 捕获已停止：{eventArgs.Exception.Message}");
        }
    }

    private sealed class HapticsProcessor
    {
        private readonly List<StereoFrame> _sourceFrames = [];
        private readonly List<byte[]> _completedChunks = [];
        private readonly Biquad _leftHighPass = new();
        private readonly Biquad _rightHighPass = new();
        private readonly Biquad _leftLowPass = new();
        private readonly Biquad _rightLowPass = new();
        private readonly Biquad[] _leftEq = [new(), new(), new(), new(), new(), new()];
        private readonly Biquad[] _rightEq = [new(), new(), new(), new(), new(), new()];
        private int _eqBandCount;
        private byte[] _chunk = new byte[BytesPerChunk];
        private AudioHapticsSettings? _appliedSettings;
        private WaveFormat? _format;
        private double _sourcePosition;
        private float _envelope;
        private float _gateGain;
        private int _chunkFrames;

        public void Reset(WaveFormat format)
        {
            _sourceFrames.Clear();
            _completedChunks.Clear();
            _chunk = new byte[BytesPerChunk];
            _format = format;
            _appliedSettings = null;
            _sourcePosition = 0;
            _envelope = 0;
            _gateGain = 0;
            _chunkFrames = 0;
            _leftHighPass.Reset();
            _rightHighPass.Reset();
            _leftLowPass.Reset();
            _rightLowPass.Reset();
            foreach (var band in _leftEq)
            {
                band.Reset();
            }
            foreach (var band in _rightEq)
            {
                band.Reset();
            }
        }

        public ProcessResult Process(
            byte[] input,
            int bytesRecorded,
            WaveFormat format,
            AudioHapticsSettings settings)
        {
            if (_format is null || _format.SampleRate != format.SampleRate ||
                _format.Channels != format.Channels || _format.BitsPerSample != format.BitsPerSample)
            {
                Reset(format);
            }
            ApplySettings(settings);
            _completedChunks.Clear();
            var inputLeftPeak = 0f;
            var inputRightPeak = 0f;
            var outputLeftPeak = 0f;
            var outputRightPeak = 0f;
            var bytesPerSample = Math.Max(1, format.BitsPerSample / 8);
            var frameBytes = bytesPerSample * Math.Max(1, format.Channels);
            var frames = bytesRecorded / frameBytes;
            for (var frame = 0; frame < frames; frame++)
            {
                var offset = frame * frameBytes;
                var left = ReadSample(input, offset, format, bytesPerSample);
                var right = format.Channels > 1
                    ? ReadSample(input, offset + bytesPerSample, format, bytesPerSample)
                    : left;
                inputLeftPeak = Math.Max(inputLeftPeak, Math.Abs(left));
                inputRightPeak = Math.Max(inputRightPeak, Math.Abs(right));
                _sourceFrames.Add(new StereoFrame(left, right));
            }

            if (_sourceFrames.Count < 2)
            {
                return new ProcessResult([], new AudioHapticsLevels(inputLeftPeak, inputRightPeak, 0, 0));
            }

            var step = format.SampleRate / (double)HapticsSampleRate;
            while (_sourcePosition + 1 < _sourceFrames.Count)
            {
                var index = (int)_sourcePosition;
                var fraction = (float)(_sourcePosition - index);
                var current = _sourceFrames[index];
                var next = _sourceFrames[index + 1];
                var left = current.Left + (next.Left - current.Left) * fraction;
                var right = current.Right + (next.Right - current.Right) * fraction;
                ProcessFrame(left, right, settings, out left, out right);
                outputLeftPeak = Math.Max(outputLeftPeak, Math.Abs(left));
                outputRightPeak = Math.Max(outputRightPeak, Math.Abs(right));
                WriteFrame(left, right);
                _sourcePosition += step;
            }
            var discard = Math.Max(0, (int)_sourcePosition - 1);
            if (discard > 0)
            {
                _sourceFrames.RemoveRange(0, discard);
                _sourcePosition -= discard;
            }

            return new ProcessResult(
                _completedChunks.ToArray(),
                new AudioHapticsLevels(inputLeftPeak, inputRightPeak, outputLeftPeak, outputRightPeak));
        }

        private void ApplySettings(AudioHapticsSettings settings)
        {
            if (ReferenceEquals(settings, _appliedSettings))
            {
                return;
            }
            _appliedSettings = settings;
            var low = Math.Clamp(settings.LowCutHz, 10, HapticsSampleRate / 2 - 10);
            var high = Math.Clamp(settings.HighCutHz, low + 10, HapticsSampleRate / 2 - 1);
            _leftHighPass.ConfigureHighPass(HapticsSampleRate, low);
            _rightHighPass.ConfigureHighPass(HapticsSampleRate, low);
            _leftLowPass.ConfigureLowPass(HapticsSampleRate, high);
            _rightLowPass.ConfigureLowPass(HapticsSampleRate, high);
            ConfigureEq(settings);
        }

        /// <summary>
        /// Configures AudioToHaptics_Mix_Mode: low shelf on the first band,
        /// peaking in the middle, high shelf on the last.
        /// </summary>
        private void ConfigureEq(AudioHapticsSettings settings)
        {
            _eqBandCount = Math.Min(settings.EqBandCount, _leftEq.Length);
            for (var band = 0; band < _eqBandCount; band++)
            {
                var frequency = settings.EqBandHz(band);
                var gainDb = Math.Clamp(settings.EqBandGainDb(band), -20, 20);
                var last = band == _eqBandCount - 1;
                foreach (var filter in new[] { _leftEq[band], _rightEq[band] })
                {
                    if (Math.Abs(gainDb) < .01)
                    {
                        filter.ConfigureBypass();
                    }
                    else if (band == 0)
                    {
                        filter.ConfigureLowShelf(HapticsSampleRate, frequency, gainDb);
                    }
                    else if (last)
                    {
                        filter.ConfigureHighShelf(HapticsSampleRate, frequency, gainDb);
                    }
                    else
                    {
                        filter.ConfigurePeaking(HapticsSampleRate, frequency, gainDb, 1.0);
                    }
                }
            }
        }

        private void ProcessFrame(
            float left,
            float right,
            AudioHapticsSettings settings,
            out float outputLeft,
            out float outputRight)
        {
            var gain = MathF.Pow(10, (float)settings.InputGainDb / 20);
            left *= gain;
            right *= gain;
            switch (settings.ChannelMode)
            {
                case "mono":
                    left = right = (left + right) * .5f;
                    break;
                case "left":
                    right = left;
                    break;
                case "right":
                    left = right;
                    break;
                case "swap":
                    (left, right) = (right, left);
                    break;
            }
            if (settings.ChannelMode is "stereo" or "swap")
            {
                var mid = (left + right) * .5f;
                var side = (left - right) * .5f * (float)(settings.StereoWidthPercent / 100);
                left = mid + side;
                right = mid - side;
            }

            left = _leftLowPass.Process(_leftHighPass.Process(left));
            right = _rightLowPass.Process(_rightHighPass.Process(right));
            // EQ sits pre-dynamics so the gate and compressor react to the
            // shaped signal, matching the reference mix stage placement.
            for (var band = 0; band < _eqBandCount; band++)
            {
                left = _leftEq[band].Process(left);
                right = _rightEq[band].Process(right);
            }
            var level = Math.Max(Math.Abs(left), Math.Abs(right));
            var attackCoefficient = TimeCoefficient(settings.AttackMs);
            var releaseCoefficient = TimeCoefficient(settings.ReleaseMs);
            var envelopeCoefficient = level > _envelope ? attackCoefficient : releaseCoefficient;
            _envelope += (level - _envelope) * envelopeCoefficient;
            var gateThreshold = MathF.Pow(10, (float)settings.GateDb / 20);
            var gateTarget = gateThreshold <= 0 || _envelope >= gateThreshold
                ? 1f
                : MathF.Pow(Math.Clamp(_envelope / gateThreshold, 0, 1), 2);
            _gateGain += (gateTarget - _gateGain) *
                (gateTarget > _gateGain ? attackCoefficient : releaseCoefficient);
            left *= _gateGain;
            right *= _gateGain;

            var ratio = (float)Math.Clamp(settings.CompressionRatio, 1, 12);
            left = Compress(left, ratio);
            right = Compress(right, ratio);
            // Source stereo channel mapping: pick the source stereo
            // channel that feeds each actuator (0 = Left, 1 = Right).
            var sourceLeft = left;
            var sourceRight = right;
            left = settings.LeftMotorSource == 1 ? sourceRight : sourceLeft;
            right = settings.RightMotorSource == 1 ? sourceRight : sourceLeft;

            // Haptics gain: linear multiplier on the haptics signal.
            var hapticsGain = (float)Math.Clamp(settings.HapticsGain, .1, 10);
            left *= hapticsGain;
            right *= hapticsGain;

            left *= (float)(settings.StrengthPercent * settings.LeftPercent / 10_000);
            right *= (float)(settings.StrengthPercent * settings.RightPercent / 10_000);
            if (settings.InvertRight)
            {
                right = -right;
            }
            var ceiling = (float)Math.Clamp(settings.CeilingPercent / 100, .05, 1);
            outputLeft = SoftLimit(left, ceiling);
            outputRight = SoftLimit(right, ceiling);
        }

        private void WriteFrame(float left, float right)
        {
            var offset = _chunkFrames * sizeof(short) * 2;
            BinaryPrimitives.WriteInt16LittleEndian(
                _chunk.AsSpan(offset, sizeof(short)), ToInt16(left));
            BinaryPrimitives.WriteInt16LittleEndian(
                _chunk.AsSpan(offset + sizeof(short), sizeof(short)), ToInt16(right));
            _chunkFrames++;
            if (_chunkFrames != FramesPerChunk)
            {
                return;
            }
            _completedChunks.Add(_chunk);
            _chunk = new byte[BytesPerChunk];
            _chunkFrames = 0;
        }

        private static float ReadSample(byte[] bytes, int offset, WaveFormat format, int bytesPerSample)
        {
            var isFloat = format.Encoding == WaveFormatEncoding.IeeeFloat ||
                          format.Encoding == WaveFormatEncoding.Extensible && format.BitsPerSample == 32;
            if (isFloat && bytesPerSample == 4)
            {
                return Math.Clamp(BitConverter.ToSingle(bytes, offset), -1f, 1f);
            }
            return bytesPerSample switch
            {
                2 => BinaryPrimitives.ReadInt16LittleEndian(bytes.AsSpan(offset, 2)) / 32768f,
                3 => ReadInt24(bytes, offset) / 8_388_608f,
                4 => BinaryPrimitives.ReadInt32LittleEndian(bytes.AsSpan(offset, 4)) / 2_147_483_648f,
                _ => 0f
            };
        }

        private static int ReadInt24(byte[] bytes, int offset)
        {
            var value = bytes[offset] | bytes[offset + 1] << 8 | bytes[offset + 2] << 16;
            return (value & 0x80_0000) != 0 ? value | unchecked((int)0xff00_0000) : value;
        }

        private static float TimeCoefficient(double milliseconds) =>
            1 - MathF.Exp(-1f / Math.Max(.0001f, (float)milliseconds / 1_000 * HapticsSampleRate));

        private static float Compress(float sample, float ratio)
        {
            const float threshold = .18f;
            var magnitude = Math.Abs(sample);
            if (magnitude <= threshold || ratio <= 1)
            {
                return sample;
            }
            var compressed = threshold + (magnitude - threshold) / ratio;
            return MathF.CopySign(compressed, sample);
        }

        private static float SoftLimit(float sample, float ceiling)
        {
            var limited = MathF.Tanh(sample / ceiling) * ceiling;
            return Math.Clamp(limited, -ceiling, ceiling);
        }

        private static short ToInt16(float sample) => (short)Math.Clamp(
            MathF.Round(sample * short.MaxValue), short.MinValue, short.MaxValue);

        private readonly record struct StereoFrame(float Left, float Right);

        public sealed record ProcessResult(IReadOnlyList<byte[]> Chunks, AudioHapticsLevels Levels);
    }

    private sealed class Biquad
    {
        private float _b0;
        private float _b1;
        private float _b2;
        private float _a1;
        private float _a2;
        private float _z1;
        private float _z2;

        public void Reset()
        {
            _z1 = 0;
            _z2 = 0;
        }

        public void ConfigureLowPass(int sampleRate, double frequency) => Configure(sampleRate, frequency, false);

        public void ConfigureHighPass(int sampleRate, double frequency) => Configure(sampleRate, frequency, true);

        public void ConfigureBypass()
        {
            _b0 = 1;
            _b1 = 0;
            _b2 = 0;
            _a1 = 0;
            _a2 = 0;
        }

        /// <summary>RBJ peaking EQ, used for the middle EQ bands.</summary>
        public void ConfigurePeaking(int sampleRate, double frequency, double gainDb, double q)
        {
            var a = Math.Pow(10, gainDb / 40);
            var omega = Omega(sampleRate, frequency);
            var cosine = Math.Cos(omega);
            var alpha = Math.Sin(omega) / (2 * Math.Max(.1, q));
            Normalize(
                1 + alpha * a, -2 * cosine, 1 - alpha * a,
                1 + alpha / a, -2 * cosine, 1 - alpha / a);
        }

        /// <summary>RBJ low shelf (S = 1), used for the lowest EQ band.</summary>
        public void ConfigureLowShelf(int sampleRate, double frequency, double gainDb)
        {
            var a = Math.Pow(10, gainDb / 40);
            var omega = Omega(sampleRate, frequency);
            var cosine = Math.Cos(omega);
            var alpha = Math.Sin(omega) / 2 * Math.Sqrt(2);
            var shared = 2 * Math.Sqrt(a) * alpha;
            Normalize(
                a * (a + 1 - (a - 1) * cosine + shared),
                2 * a * (a - 1 - (a + 1) * cosine),
                a * (a + 1 - (a - 1) * cosine - shared),
                a + 1 + (a - 1) * cosine + shared,
                -2 * (a - 1 + (a + 1) * cosine),
                a + 1 + (a - 1) * cosine - shared);
        }

        /// <summary>RBJ high shelf (S = 1), used for the highest EQ band.</summary>
        public void ConfigureHighShelf(int sampleRate, double frequency, double gainDb)
        {
            var a = Math.Pow(10, gainDb / 40);
            var omega = Omega(sampleRate, frequency);
            var cosine = Math.Cos(omega);
            var alpha = Math.Sin(omega) / 2 * Math.Sqrt(2);
            var shared = 2 * Math.Sqrt(a) * alpha;
            Normalize(
                a * (a + 1 + (a - 1) * cosine + shared),
                -2 * a * (a - 1 + (a + 1) * cosine),
                a * (a + 1 + (a - 1) * cosine - shared),
                a + 1 - (a - 1) * cosine + shared,
                2 * (a - 1 - (a + 1) * cosine),
                a + 1 - (a - 1) * cosine - shared);
        }

        private static double Omega(int sampleRate, double frequency) =>
            2 * Math.PI * Math.Clamp(frequency, 10, sampleRate / 2.0 - 1) / sampleRate;

        private void Normalize(double b0, double b1, double b2, double a0, double a1, double a2)
        {
            _b0 = (float)(b0 / a0);
            _b1 = (float)(b1 / a0);
            _b2 = (float)(b2 / a0);
            _a1 = (float)(a1 / a0);
            _a2 = (float)(a2 / a0);
        }

        public float Process(float sample)
        {
            var output = sample * _b0 + _z1;
            _z1 = sample * _b1 + _z2 - _a1 * output;
            _z2 = sample * _b2 - _a2 * output;
            return output;
        }

        private void Configure(int sampleRate, double frequency, bool highPass)
        {
            var omega = 2 * Math.PI * Math.Clamp(frequency, 10, sampleRate / 2.0 - 1) / sampleRate;
            var cosine = Math.Cos(omega);
            var alpha = Math.Sin(omega) / Math.Sqrt(2);
            var a0 = 1 + alpha;
            var b0 = highPass ? (1 + cosine) / 2 : (1 - cosine) / 2;
            var b1 = highPass ? -(1 + cosine) : 1 - cosine;
            var b2 = b0;
            _b0 = (float)(b0 / a0);
            _b1 = (float)(b1 / a0);
            _b2 = (float)(b2 / a0);
            _a1 = (float)(-2 * cosine / a0);
            _a2 = (float)((1 - alpha) / a0);
        }
    }
}
