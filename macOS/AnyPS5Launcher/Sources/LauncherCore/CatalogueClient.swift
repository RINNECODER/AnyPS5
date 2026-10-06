import Foundation

public actor CatalogueClient {
    public static let endpoint = URL(string: "https://raw.githubusercontent.com/saawant12/orbit-store-ps5/main/catalogue-v3.enc")!
    private let session: URLSession
    private let cacheURL: URL

    public init(cacheURL: URL, session: URLSession = .shared) {
        self.cacheURL = cacheURL
        self.session = session
    }

    public func refresh() async throws -> CatalogueSnapshot {
        do {
            var request = URLRequest(url: Self.endpoint)
            request.timeoutInterval = 30
            request.cachePolicy = .reloadIgnoringLocalCacheData
            let (data, response) = try await session.data(for: request)
            guard let response = response as? HTTPURLResponse, response.statusCode == 200 else {
                throw LauncherError("Orbit catalogue server returned an unsuccessful response.")
            }
            let plaintext = try OrbitFeedEnvelope.decode(data)
            let catalogue = try OrbitCatalogue.decode(plaintext)
            let date = Date()
            var warning: String?
            do {
                try FileManager.default.createDirectory(at: cacheURL.deletingLastPathComponent(), withIntermediateDirectories: true)
                try plaintext.write(to: cacheURL, options: .atomic)
            } catch { warning = "Catalogue loaded, but its offline cache could not be saved: \(error.localizedDescription)" }
            return CatalogueSnapshot(catalogue: catalogue, fetchedAt: date, isCached: false, warning: warning)
        } catch {
            guard let data = try? Data(contentsOf: cacheURL), let catalogue = try? OrbitCatalogue.decode(data) else { throw error }
            let attributes = try? FileManager.default.attributesOfItem(atPath: cacheURL.path)
            let date = attributes?[.modificationDate] as? Date ?? .distantPast
            return CatalogueSnapshot(catalogue: catalogue, fetchedAt: date, isCached: true,
                                     warning: "Showing the saved catalogue. Refresh failed: \(error.localizedDescription)")
        }
    }
}
